/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_app.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/bind/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <cctype>
#include <chrono>
#include <ctime>
#include <sstream>

#include "3gpp_29.500.h"
#include "logger.hpp"
#include "nef_audit_log.hpp"
#include "nef_callback_uri_validator.hpp"
#include "nef_input_validation.hpp"
#include "nef_pfd_atomicity.hpp"
#include "nef_pfd_management_quality.hpp"
#include "nef_client.hpp"
#include "nef_config.hpp"
#include "nef_jwt.hpp"
#include "nef_notification_mapper.hpp"
#include "nef_async_parse_helpers.hpp"
#include "nef_sbi_helper.hpp"
#include "nef_sbi_response_policy.hpp"

#include "AmfCreatedEventSubscription.h"
#include "AsSessionWithQoSSubscription.h"
#include "UserPlaneEvent.h"
#include "BdtPolicy.h"
#include "Helpers.h"
#include "NefEvent_anyOf.h"
#include "NefEventExposureSubsc.h"
#include "TrafficInfluData.h"
#include "TrafficInfluDataPatch.h"

#include <algorithm>
#include "nef_app_internal.hpp"

using namespace oai::nef::app;
using namespace boost::placeholders;
using namespace oai::common::sbi;

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

// Traffic Influence: read one subscription
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_get(
    const std::string& af_id, const std::string& app_session_id,
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          af_id, NEF_SERVICE_TRAFFIC_INFLUENCE, response_body, http_code)) {
    return;
  }
  std::shared_lock lock(m_ti_mutex);
  auto it = m_ti_sessions.find(app_session_id);
  if (it == m_ti_sessions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "TI session not found");
    return;
  }
  auto owner_it = m_ti_id2af_id.find(app_session_id);
  if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN,
        "AF is not allowed to access this resource");
    return;
  }
  response_body              = it->second;
  response_body["afTransId"] = app_session_id;
  http_code                  = http_status_code::OK;
}

// Traffic Influence: list the AF's subscriptions
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          af_id, NEF_SERVICE_TRAFFIC_INFLUENCE, response_body, http_code)) {
    return;
  }
  std::shared_lock lock(m_ti_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [id, session] : m_ti_sessions) {
    auto owner_it = m_ti_id2af_id.find(id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) continue;
    nlohmann::json entry = session;
    entry["afTransId"]   = id;
    response_body.push_back(entry);
  }
  http_code = http_status_code::OK;
}

//------------------------------------------------------------------------------
// traffic_influence_update: authorizes, parses and validates the body,
// SSRF-checks the callback URI, checks the owner and looks up pcf_policy_id,
// then sends the policy-auth update to PCF without waiting for it.
// cont_ti_update handles a PCF failure and, on success, overwrites the stored
// session and answers 200.
void nef_app::ti_update(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_af(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE, sink)) {
    return;
  }

  oai::_3gpp::model::TrafficInfluData ti;
  try {
    from_json(body, ti);
    ti.validate();
  } catch (const nlohmann::json::exception& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY,
            std::string("Validation failed: ") + e.what())
            .dump());
  } catch (const std::exception& e) {
    // e.g. std::invalid_argument thrown by a generated enum from_json on an
    // unrecognised value. Treat as a malformed request body (400) rather than
    // letting it escape and abort the process.
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            std::string("Invalid body: ") + e.what())
            .dump());
  }

  if (!ti.afAppIdIsSet() && !ti.trafficFiltersIsSet() &&
      !ti.ethTrafficFiltersIsSet()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
            "required")
            .dump());
  }

  {
    std::string err;
    if (err.empty()) err = validate_string_field(body, "afAppId", false, 256);
    if (err.empty()) err = validate_string_field(body, "dnn", false, 100);
    if (err.empty())
      err = validate_string_field(body, "notificationDestination", false, 2048);
    if (!err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, err)
              .dump());
    }
  }

  if (body.contains("notificationDestination") &&
      body["notificationDestination"].is_string()) {
    const std::string uri_err = validate_callback_uri(
        body["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST,
              "notificationDestination: " + uri_err)
              .dump());
    }
  }

  std::string pcf_policy_id;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "TI session not found")
              .dump());
    }
    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN,
              "AF is not allowed to access this resource")
              .dump());
    }
    auto pcf_it = m_ti_id2pcf_policy_id.find(ti_id);
    if (pcf_it != m_ti_id2pcf_policy_id.end()) pcf_policy_id = pcf_it->second;
  }

  if (pcf_policy_id.empty()) {
    Logger::nef_app().warn(
        "No PCF policy ID found for TI session %s", ti_id.c_str());
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_GATEWAY,
        make_problem_detail(
            http_status_code::BAD_GATEWAY,
            "Missing PCF policy identifier for TI session")
            .dump());
  }
  clear_request_bearer_token();

  // Send the policy-auth update to PCF.
  m_nef_client->update_pcf_policy_auth_async(
      pcf_policy_id, body,
      [this, af_id, ti_id, body,
       sink = std::move(sink)](oai::nghttp2::response r) mutable {
        cont_ti_update(af_id, ti_id, body, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_update(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, oai::nghttp2::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_update ti_id=%s status=%d", ti_id.c_str(), r.status_code);
  // A PCF failure fails the whole request with 502 (504 on a timeout).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "PCF TI update failed for ti_id=%s (http=%d)", ti_id.c_str(),
        r.status_code);
    const int code = sbi_error_http_code(r);
    return sink(
        code,
        make_problem_detail(
            code, "Bad Gateway", "Failed to update policy authorization in PCF")
            .dump());
  }

  // Commit the new session body
  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "TI session not found")
              .dump());
    }
    session_it->second = body;
  }
  nef_audit::log("UPDATE", "TI", af_id, ti_id, http_status_code::OK);
  sink(http_status_code::OK, body.dump());
}

//------------------------------------------------------------------------------
// traffic_influence_patch: authorizes, parses and validates the patch, checks
// the owner, applies merge_patch to a local patched_copy and looks up
// pcf_policy_id, then sends the policy-auth update to PCF without waiting for
// it. cont_ti_patch handles a PCF failure and, on success, overwrites the
// stored session and answers 200.
void nef_app::ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& patch_body, const std::string& token,
    response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_af(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE, sink)) {
    return;
  }

  oai::_3gpp::model::TrafficInfluDataPatch ti_patch;
  try {
    from_json(patch_body, ti_patch);
    ti_patch.validate();
  } catch (const nlohmann::json::exception& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY,
            std::string("Validation failed: ") + e.what())
            .dump());
  } catch (const std::exception& e) {
    // e.g. std::invalid_argument thrown by a generated enum from_json on an
    // unrecognised value. Treat as a malformed request body (400) rather than
    // letting it escape and abort the process.
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            std::string("Invalid body: ") + e.what())
            .dump());
  }

  std::string pcf_policy_id;
  nlohmann::json patched_copy;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "TI session not found")
              .dump());
    }
    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN,
              "AF is not allowed to access this resource")
              .dump());
    }
    auto pcf_it = m_ti_id2pcf_policy_id.find(ti_id);
    if (pcf_it != m_ti_id2pcf_policy_id.end()) pcf_policy_id = pcf_it->second;
    patched_copy = session_it->second;
    patched_copy.merge_patch(patch_body);
  }

  if (pcf_policy_id.empty()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_GATEWAY,
        make_problem_detail(
            http_status_code::BAD_GATEWAY,
            "Missing PCF policy identifier for TI session")
            .dump());
  }
  clear_request_bearer_token();

  // Send the merged copy to PCF as the policy-auth update.
  m_nef_client->update_pcf_policy_auth_async(
      pcf_policy_id, patched_copy,
      [this, af_id, ti_id, patched_copy,
       sink = std::move(sink)](oai::nghttp2::response r) mutable {
        cont_ti_patch(
            af_id, ti_id, ti_id, std::move(patched_copy), std::move(r),
            std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const std::string& app_session_id, nlohmann::json patched_copy,
    oai::nghttp2::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_patch ti_id=%s status=%d", app_session_id.c_str(),
      r.status_code);
  // A PCF failure fails the whole request with 502 (504 on a timeout).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "PCF TI patch failed for ti_id=%s (http=%d)", app_session_id.c_str(),
        r.status_code);
    const int code = sbi_error_http_code(r);
    return sink(
        code,
        make_problem_detail(
            code, "Bad Gateway", "Failed to update policy authorization in PCF")
            .dump());
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(app_session_id);
    if (session_it == m_ti_sessions.end()) {
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "TI session not found")
              .dump());
    }
    session_it->second = patched_copy;
  }
  nlohmann::json response_body = std::move(patched_copy);
  response_body["afTransId"]   = app_session_id;
  nef_audit::log("PATCH", "TI", af_id, app_session_id, http_status_code::OK);
  sink(http_status_code::OK, response_body.dump());
}

//------------------------------------------------------------------------------
// traffic_influence_create: a PCF call followed by a UDR call.
//
// Both endpoints are looked up first, here on the dispatcher worker, and
// passed down by value so the continuations can use the discovery-free
// *_at_async variants. This matters because discover_nf must never run on
// oai-http-io: it would deadlock against its own pool.
//
// ti_create           authorizes and validates the request, stores the
//                     session, adds the subscription, looks up both
//                     endpoints, and sends the create to PCF.
// cont_ti_create_pcf  on a PCF error, fails the request with 502 and rolls
//                     back only local state, since PCF committed nothing. On
//                     success it fills in the id maps and sends the PUT to
//                     UDR.
// cont_ti_create_udr  is best-effort: a UDR failure is logged, and the AF
//                     gets its 201 with the echoed body either way.
void nef_app::ti_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info("Create TI subscription for AF: %s", af_id.c_str());

  if (reject_unauthorized_af(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE, sink)) {
    return;
  }

  oai::_3gpp::model::TrafficInfluData ti;
  try {
    from_json(body, ti);
    ti.validate();
  } catch (const nlohmann::json::exception& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY,
            std::string("Validation failed: ") + e.what())
            .dump());
  } catch (const std::exception& e) {
    // e.g. std::invalid_argument thrown by a generated enum from_json on an
    // unrecognised value. Treat as a malformed request body (400) rather than
    // letting it escape and abort the process.
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            std::string("Invalid body: ") + e.what())
            .dump());
  }

  if (!ti.afAppIdIsSet() && !ti.trafficFiltersIsSet() &&
      !ti.ethTrafficFiltersIsSet()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
            "required")
            .dump());
  }

  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty()) err = validate_string_field(body, "afAppId", false, 256);
    if (err.empty()) err = validate_string_field(body, "dnn", false, 100);
    if (err.empty())
      err = validate_string_field(body, "notificationDestination", false, 2048);
    if (!err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, err)
              .dump());
    }
  }

  if (body.contains("notificationDestination") &&
      body["notificationDestination"].is_string()) {
    const std::string uri_err = validate_callback_uri(
        body["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST,
              "notificationDestination: " + uri_err)
              .dump());
    }
  }

  std::string ti_id;
  generate_af_subscription_id(ti_id);
  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    m_ti_sessions[ti_id] = body;
    m_ti_id2af_id[ti_id] = af_id;
  }

  auto ti_sub = std::make_shared<nef_subscription>(m_event_sub);
  ti_sub->set_af_subscription_id(ti_id);
  ti_sub->set_scs_as_id(af_id);
  ti_sub->set_service_type(
      nef_service_type_t::NEF_SERVICE_TYPE_TRAFFIC_INFLUENCE);
  ti_sub->set_subscription_data(body);
  if (body.contains("notificationDestination") &&
      body["notificationDestination"].is_string()) {
    ti_sub->set_notification_uri(
        body["notificationDestination"].get<std::string>());
  }
  add_subscription(ti_id, ti_sub);

  // Look up both NF endpoints now, on the dispatcher worker.
  std::string pcf_ep, udr_ep;
  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_PCF, pcf_ep) ||
      !m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, udr_ep)) {
    Logger::nef_app().warn(
        "TI create: NF discovery failed (PCF/UDR), rolling back ti_id=%s",
        ti_id.c_str());
    {
      const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
      m_ti_sessions.erase(ti_id);
      m_ti_id2af_id.erase(ti_id);
      m_ti_id2pcf_policy_id.erase(ti_id);
    }
    remove_subscription(ti_id);
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_GATEWAY,
        make_problem_detail(
            http_status_code::BAD_GATEWAY,
            "Failed to create policy authorization in PCF")
            .dump());
  }
  clear_request_bearer_token();

  // Send the create to PCF with the discovery-free *_at_async variant.
  m_nef_client->create_pcf_policy_auth_at_async(
      pcf_ep, body,
      [this, af_id, body, ti_id, pcf_ep, udr_ep, ti_sub,
       sink = std::move(sink)](oai::nghttp2::response r) mutable {
        cont_ti_create_pcf(
            af_id, body, ti_id, pcf_ep, udr_ep, ti_sub, std::move(r),
            std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_create_pcf(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& ti_id, const std::string& pcf_ep,
    const std::string& udr_ep, std::shared_ptr<nef_subscription> ti_sub,
    oai::nghttp2::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_create_pcf ti_id=%s status=%d", ti_id.c_str(), r.status_code);
  const std::string pcf_policy_id =
      sbi_ok(r) ? nef_async_parse_pcf_app_session_id(r) : "";
  // A PCF failure fails the request with 502 (504 on a timeout). Roll back
  // only our own state: PCF committed nothing, so there is nothing southbound
  // to undo.
  if (!sbi_ok(r) || pcf_policy_id.empty()) {
    Logger::nef_app().warn(
        "PCF TI create failed for ti_id=%s (http=%d), rolling back local "
        "session",
        ti_id.c_str(), r.status_code);
    {
      const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
      m_ti_sessions.erase(ti_id);
      m_ti_id2af_id.erase(ti_id);
      m_ti_id2pcf_policy_id.erase(ti_id);
    }
    remove_subscription(ti_id);
    const int code = sbi_error_http_code(r);
    return sink(
        code,
        make_problem_detail(
            code, "Bad Gateway", "Failed to create policy authorization in PCF")
            .dump());
  }

  // A concurrent AF delete may have removed ti_id while the PCF call was in
  // progress. If so, do not recreate it: the AF delete already won. Send a
  // best-effort async delete for the PCF app-session just created, and answer
  // 204.
  //
  // The presence check and the m_ti_id2pcf_policy_id update happen under
  // m_ti_mutex. The compensating PCF delete is sent outside that lock (never
  // send an SBI call while holding a store mutex), using the discovery-free
  // *_at_async variant on the pcf_ep looked up earlier.
  bool ti_vanished = false;
  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    if (m_ti_sessions.find(ti_id) == m_ti_sessions.end()) {
      ti_vanished = true;
    } else {
      m_ti_id2pcf_policy_id[ti_id] = pcf_policy_id;
    }
  }
  if (ti_vanished) {
    Logger::nef_app().info(
        "TI %s vanished during PCF create (concurrent delete); compensating",
        ti_id.c_str());
    m_nef_client->delete_pcf_policy_auth_at_async(
        pcf_ep, pcf_policy_id, [](oai::nghttp2::response) {});
    return sink(http_status_code::NO_CONTENT, "");
  }

  // Map PCF policy ID → NEF sub ID so PCF notifications reach the right
  // subscription. ti_sub is the same shared_ptr that add_subscription stored,
  // so the notification path sees this update.
  ti_sub->set_nf_subscription_id(pcf_policy_id);
  {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[pcf_policy_id] = ti_id;
  }

  // Send the influence-data PUT to UDR with the discovery-free *_at_async
  // variant, on the udr_ep looked up earlier.
  m_nef_client->udr_put_influence_data_at_async(
      udr_ep, ti_id, body,
      [this, body, ti_id,
       sink = std::move(sink)](oai::nghttp2::response ur) mutable {
        cont_ti_create_udr(body, ti_id, std::move(ur), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_create_udr(
    const nlohmann::json& body, const std::string& ti_id,
    oai::nghttp2::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_create_udr ti_id=%s status=%d", ti_id.c_str(), r.status_code);
  // Best-effort step: a UDR failure is logged and otherwise ignored, and the
  // AF still gets its 201.
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR influence PUT failed for ti_id=%s (http=%d)", ti_id.c_str(),
        r.status_code);
  }
  nlohmann::json response_body = body;
  response_body["afTransId"]   = ti_id;
  // af_id is needed only for the audit log; recover it from the owner map.
  std::string af_id;
  {
    std::shared_lock lock(m_ti_mutex);
    auto it = m_ti_id2af_id.find(ti_id);
    if (it != m_ti_id2af_id.end()) af_id = it->second;
  }
  nef_audit::log("CREATE", "TI", af_id, ti_id, http_status_code::CREATED);
  sink(http_status_code::CREATED, response_body.dump());
}
//------------------------------------------------------------------------------
// traffic_influence_delete: a PCF app-session DELETE followed by a UDR
// influence DELETE.
//
// Both southbound calls are best-effort: a failure only logs a warning, and
// once the checks pass the AF always gets a 204 with an empty body. The chain
// is idempotent and needs no compensation.
//
// ti_delete           does the authorize, not-found and owner checks, reads
//                     the PCF policy id, looks up the PCF and UDR endpoints,
//                     and sends the PCF app-session DELETE, but only when a
//                     PCF policy exists.
// cont_ti_delete_pcf  sends the UDR influence DELETE, or skips it when UDR
//                     could not be discovered.
// cont_ti_delete_udr  erases the local state and sends the 204. The erase
//                     happens here, in the last continuation, after both SBI
//                     calls.
void nef_app::ti_delete(
    const std::string& af_id, const std::string& ti_id,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }

  std::string pcf_policy_id;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(http_status_code::FORBIDDEN, "");
    }
    auto pcf_it = m_ti_id2pcf_policy_id.find(ti_id);
    if (pcf_it != m_ti_id2pcf_policy_id.end()) pcf_policy_id = pcf_it->second;
  }

  // Look up both NF endpoints now, on the dispatcher worker.
  std::string pcf_ep, udr_ep;
  const bool pcf_ok = pcf_policy_id.empty() ||
                      m_nef_client->discover_nf(nf_type_t::NF_TYPE_PCF, pcf_ep);
  const bool udr_ok = m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, udr_ep);
  clear_request_bearer_token();

  if (pcf_policy_id.empty() || !pcf_ok) {
    // No PCF policy to delete, or PCF could not be discovered: skip the PCF
    // call and go straight to the UDR call. A PCF discovery failure only logs
    // a warning (best-effort, like a failed PCF delete).
    if (!pcf_policy_id.empty() && !pcf_ok) {
      Logger::nef_app().warn(
          "PCF TI delete: PCF discovery failed for ti_id=%s policy_id=%s",
          ti_id.c_str(), pcf_policy_id.c_str());
    }
    // Finish the PCF step inline with a status-0 placeholder response (nothing
    // is sent southbound); cont_ti_delete_pcf then runs the UDR call.
    return cont_ti_delete_pcf(
        af_id, ti_id, udr_ok ? udr_ep : std::string{}, oai::nghttp2::response{},
        std::move(sink));
  }

  // Send the PCF app-session DELETE with the discovery-free *_at_async.
  m_nef_client->delete_pcf_policy_auth_at_async(
      pcf_ep, pcf_policy_id,
      [this, af_id, ti_id, ti_policy = pcf_policy_id,
       udr_ep = (udr_ok ? udr_ep : std::string{}),
       sink   = std::move(sink)](oai::nghttp2::response r) mutable {
        if (!sbi_ok(r)) {
          Logger::nef_app().warn(
              "PCF TI delete failed for ti_id=%s policy_id=%s (http=%d)",
              ti_id.c_str(), ti_policy.c_str(), r.status_code);
        }
        cont_ti_delete_pcf(af_id, ti_id, udr_ep, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_delete_pcf(
    const std::string& af_id, const std::string& ti_id,
    const std::string& udr_ep, oai::nghttp2::response /*r*/,
    response_sink sink) {
  // The caller has already logged or skipped the PCF call. Now send the UDR
  // influence DELETE. An empty udr_ep means UDR could not be discovered: skip
  // the call with a warning (best-effort) and go straight to the final step.
  if (udr_ep.empty()) {
    Logger::nef_app().warn(
        "UDR influence DELETE skipped for ti_id=%s (UDR undiscoverable)",
        ti_id.c_str());
    return cont_ti_delete_udr(
        af_id, ti_id, oai::nghttp2::response{}, std::move(sink));
  }
  m_nef_client->udr_delete_influence_data_at_async(
      udr_ep, ti_id,
      [this, af_id, ti_id,
       sink = std::move(sink)](oai::nghttp2::response ur) mutable {
        cont_ti_delete_udr(af_id, ti_id, std::move(ur), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_delete_udr(
    const std::string& af_id, const std::string& ti_id,
    oai::nghttp2::response r, response_sink sink) {
  // Best-effort: a UDR failure only logs a warning and does not change the
  // 204. r.status_code == 0 means the UDR call was skipped, which the caller
  // has already logged, so do not log a second warning in that case.
  if (r.status_code != 0 && !sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR influence DELETE failed for ti_id=%s (http=%d)", ti_id.c_str(),
        r.status_code);
  }

  // Erase the local state only now, after both SBI calls. Read the PCF policy
  // id under the same lock so m_nf2af_sub_id can be cleaned up too.
  std::string pcf_policy_id;
  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto pcf_it = m_ti_id2pcf_policy_id.find(ti_id);
    if (pcf_it != m_ti_id2pcf_policy_id.end()) pcf_policy_id = pcf_it->second;
    m_ti_sessions.erase(ti_id);
    m_ti_id2af_id.erase(ti_id);
    m_ti_id2pcf_policy_id.erase(ti_id);
  }
  if (!pcf_policy_id.empty()) {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(pcf_policy_id);
  }
  remove_subscription(ti_id);
  nef_audit::log("DELETE", "TI", af_id, ti_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}
