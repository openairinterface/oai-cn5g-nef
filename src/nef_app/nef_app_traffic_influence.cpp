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

// TI GET
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

// TI LIST
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
// traffic_influence_update. The pre-southbound block — authorize, parse,
// validate, SSRF-check the callback URI, look up the owner and resolve
// pcf_policy_id — is unchanged. Only the PCF policy-auth update becomes an
// async fire. cont_ti_update now owns the failure branch and, on success, the
// post-commit session overwrite and the 200.
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

  // Fire the PCF policy-auth update.
  m_nef_client->update_pcf_policy_auth_async(
      pcf_policy_id, body,
      [this, af_id, ti_id, body,
       sink = std::move(sink)](oai::sba::response r) mutable {
        cont_ti_update(af_id, ti_id, body, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_update(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_update ti_id=%s status=%d", ti_id.c_str(), r.status_code);
  // FATAL-502: a PCF failure fails the whole request (504 on a timeout).
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
// traffic_influence_patch. The pre-southbound block — authorize, typed-parse
// and validate the patch, look up the owner, merge_patch into a local
// patched_copy and resolve pcf_policy_id — is unchanged. Only the PCF
// policy-auth update becomes an async fire. cont_ti_patch owns the failure
// branch and, on success, the post-commit session overwrite and the 200.
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

  // Fire the PCF policy-auth update with the merged copy.
  m_nef_client->update_pcf_policy_auth_async(
      pcf_policy_id, patched_copy,
      [this, af_id, ti_id, patched_copy,
       sink = std::move(sink)](oai::sba::response r) mutable {
        cont_ti_patch(
            af_id, ti_id, ti_id, std::move(patched_copy), std::move(r),
            std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const std::string& app_session_id, nlohmann::json patched_copy,
    oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_patch ti_id=%s status=%d", app_session_id.c_str(),
      r.status_code);
  // FATAL-502: a PCF failure fails the whole request (504 on a timeout).
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
// traffic_influence_create: a PCF call chained into a UDR call.
//
// Both endpoints are resolved up front, here on the dispatcher worker, and
// passed down by value so the continuations can use the discovery-free
// *_at_async variants. That is the point of the arrangement: discover_nf must
// never run on oai-http-io, or it would deadlock against its own pool.
//
// ti_create           does the usual pre-southbound work, adds the
//                     subscription, resolves both endpoints, and fires the
//                     PCF create.
// cont_ti_create_pcf  fails the request with 502 on a PCF error, rolling back
//                     only local state since PCF committed nothing. On
//                     success it wires up the id maps and fires the UDR put.
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

  // pre-resolve BOTH NF endpoints HERE (dispatcher worker).
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

  // PCF create via the discovery-free *_at_async variant.
  m_nef_client->create_pcf_policy_auth_at_async(
      pcf_ep, body,
      [this, af_id, body, ti_id, pcf_ep, udr_ep, ti_sub,
       sink = std::move(sink)](oai::sba::response r) mutable {
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
    oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_create_pcf ti_id=%s status=%d", ti_id.c_str(), r.status_code);
  const std::string pcf_policy_id =
      sbi_ok(r) ? nef_async_parse_pcf_app_session_id(r) : "";
  // FATAL-502 (504 on a timeout). Roll back only our own state: PCF never
  // committed anything, so there is nothing southbound to compensate.
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

  // A concurrent AF delete may have removed ti_id while PCF was in flight. If
  // it is gone, do not resurrect it: the AF delete already won, so best-effort
  // async-delete the PCF app-session we just created and complete with 204.
  //
  // The presence check and the surviving wiring happen under m_ti_mutex. The
  // compensating southbound delete is FIRED OUTSIDE that lock — never fire an
  // SBI call while holding a store mutex — and uses the discovery-free
  // *_at_async variant on the pre-resolved pcf_ep.
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
        pcf_ep, pcf_policy_id, [](oai::sba::response) {});
    return sink(http_status_code::NO_CONTENT, "");
  }

  // Wire PCF policy ID → NEF sub ID for the notification return path. ti_sub
  // is the same shared_ptr that add_subscription stored, so the notification
  // path observes this re-set.
  ti_sub->set_nf_subscription_id(pcf_policy_id);
  {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[pcf_policy_id] = ti_id;
  }

  // UDR put-influence via the discovery-free *_at_async variant
  // on the pre-resolved udr_ep.
  m_nef_client->udr_put_influence_data_at_async(
      udr_ep, ti_id, body,
      [this, body, ti_id,
       sink = std::move(sink)](oai::sba::response ur) mutable {
        cont_ti_create_udr(body, ti_id, std::move(ur), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_create_udr(
    const nlohmann::json& body, const std::string& ti_id, oai::sba::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_create_udr ti_id=%s status=%d", ti_id.c_str(), r.status_code);
  // Best-effort leg: a UDR failure is logged and otherwise ignored — the AF
  // still gets its 201.
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
// traffic_influence_delete: a PCF app-session DELETE chained into a UDR
// influence DELETE.
//
// BEST-EFFORT throughout. Both southbound failures are warn-only and the AF
// always gets a 204 with an empty body. The chain is idempotent and needs no
// compensation.
//
// ti_delete           does the authorize / not-found / owner checks, captures
//                     the PCF policy id, resolves the PCF and UDR endpoints,
//                     and fires the PCF app-session DELETE — only when a PCF
//                     policy exists.
// cont_ti_delete_pcf  fires the UDR influence DELETE, skipping it when UDR
//                     turned out to be undiscoverable.
// cont_ti_delete_udr  erases the local state and sends the 204. The erase
//                     happens here, in the FINAL continuation after both SBI
//                     calls, which preserves the sync ordering.
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

  // Resolve BOTH NF endpoints HERE (dispatcher worker).
  std::string pcf_ep, udr_ep;
  const bool pcf_ok = pcf_policy_id.empty() ||
                      m_nef_client->discover_nf(nf_type_t::NF_TYPE_PCF, pcf_ep);
  const bool udr_ok = m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, udr_ep);
  clear_request_bearer_token();

  if (pcf_policy_id.empty() || !pcf_ok) {
    // No PCF policy to delete (or PCF undiscoverable) — skip the PCF leg, go
    // straight to the UDR leg. A PCF discovery failure is warn-only
    // (best-effort, mirrors the sync PCF-failure-is-warn-only branch).
    if (!pcf_policy_id.empty() && !pcf_ok) {
      Logger::nef_app().warn(
          "PCF TI delete: PCF discovery failed for ti_id=%s policy_id=%s",
          ti_id.c_str(), pcf_policy_id.c_str());
    }
    // Inline-finish the PCF leg with a status-0 sentinel (no southbound fire),
    // then the UDR leg runs from cont_ti_delete_pcf's pass-through.
    return cont_ti_delete_pcf(
        af_id, ti_id, udr_ok ? udr_ep : std::string{}, oai::sba::response{},
        std::move(sink));
  }

  // PCF app-session DELETE via the discovery-free *_at_async.
  m_nef_client->delete_pcf_policy_auth_at_async(
      pcf_ep, pcf_policy_id,
      [this, af_id, ti_id, ti_policy = pcf_policy_id,
       udr_ep = (udr_ok ? udr_ep : std::string{}),
       sink   = std::move(sink)](oai::sba::response r) mutable {
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
    const std::string& udr_ep, oai::sba::response /*r*/, response_sink sink) {
  // The PCF leg was already logged, or skipped, by the caller. Now fire the
  // UDR influence DELETE. An empty udr_ep means UDR was undiscoverable: skip
  // the leg with a warning (best-effort) and go straight to the final step.
  if (udr_ep.empty()) {
    Logger::nef_app().warn(
        "UDR influence DELETE skipped for ti_id=%s (UDR undiscoverable)",
        ti_id.c_str());
    return cont_ti_delete_udr(
        af_id, ti_id, oai::sba::response{}, std::move(sink));
  }
  m_nef_client->udr_delete_influence_data_at_async(
      udr_ep, ti_id,
      [this, af_id, ti_id,
       sink = std::move(sink)](oai::sba::response ur) mutable {
        cont_ti_delete_udr(af_id, ti_id, std::move(ur), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_ti_delete_udr(
    const std::string& af_id, const std::string& ti_id, oai::sba::response r,
    response_sink sink) {
  // Best-effort: a UDR failure is warn-only and does not change the 204.
  // r.status_code == 0 means the UDR leg was skipped, which the caller has
  // already logged — do not emit a spurious second WARN in that case.
  if (r.status_code != 0 && !sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR influence DELETE failed for ti_id=%s (http=%d)", ti_id.c_str(),
        r.status_code);
  }

  // Erase the local state only now, AFTER both SBI calls — that preserves the
  // sync ordering. Recover the PCF policy id under the same lock so
  // m_nf2af_sub_id can be cleaned too.
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
