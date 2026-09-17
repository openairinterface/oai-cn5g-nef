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

//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          af_id, NEF_SERVICE_BDT, response_body, http_code)) {
    return;
  }

  std::shared_lock lock(m_bdt_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [id, policy] : m_bdt_sessions) {
    auto owner_it = m_bdt_id2af_id.find(id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      continue;
    }
    nlohmann::json entry;
    to_json(entry, policy);
    entry["bdtRefId"] = id;
    response_body.push_back(entry);
  }
  http_code = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_get(
    const std::string& af_id, const std::string& bdt_id,
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          af_id, NEF_SERVICE_BDT, response_body, http_code)) {
    return;
  }

  std::shared_lock lock(m_bdt_mutex);
  auto it = m_bdt_sessions.find(bdt_id);
  if (it == m_bdt_sessions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "BDT policy not found");
    return;
  }

  auto owner_it = m_bdt_id2af_id.find(bdt_id);
  if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN,
        "AF is not allowed to access this resource");
    return;
  }

  to_json(response_body, it->second);
  response_body["bdtRefId"] = bdt_id;
  http_code                 = http_status_code::OK;
}

//------------------------------------------------------------------------------
// bdt_policy_create. Authorize, typed-parse, validate bdtPolData, check the
// path params, generate bdt_id and store it locally — all unchanged from the
// sync handler. Only the PCF BDT create becomes an async fire, and
// cont_bdt_create builds the 201 body.
//
// SUCCESS-ON-3xx: PCF answers a successful create with 303, so a 303 counts as
// success here, exactly like the sync `!= SEE_OTHER` guard. A genuine failure
// rolls back the local state and answers 502.
//
// The sync handler never parsed the PCF id into the AF response; it only
// stored pcf_bdt_id in m_bdt_id2pcf_policy_id. cont_bdt_create extracts that
// id with nef_async_parse_pcf_bdt_policy_id, whose precedence mirrors the sync
// create_pcf_bdt_policy exactly:
//   1. last path segment of the Location header,
//   2. body bdtPolicyId,
//   3. body bdtRefId,
//   4. body bdtPolData.bdtRefId.
// Location must come first. The appSessionId-first app-session helper is the
// wrong one here: with it, a PCF response carrying only a body would leave the
// map unwired and break every later BDT update, patch and delete.
void nef_app::bdt_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info("BDT policy create for AF: %s", af_id.c_str());

  if (reject_unauthorized_af(af_id, NEF_SERVICE_BDT, sink)) return;

  oai::_3gpp::model::BdtPolicy bdt_policy;
  try {
    from_json(body, bdt_policy);
    bdt_policy.validate();
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

  if (!bdt_policy.bdtPolDataIsSet()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Missing required field: bdtPolData")
            .dump());
  }
  {
    const std::string err = validate_string_param(af_id, "afId", 256);
    if (!err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, err)
              .dump());
    }
  }

  std::string bdt_id;
  generate_af_subscription_id(bdt_id);
  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    m_bdt_sessions[bdt_id] = bdt_policy;
    m_bdt_id2af_id[bdt_id] = af_id;
  }
  clear_request_bearer_token();

  // Fire the PCF BDT create. A 303 counts as success.
  m_nef_client->create_pcf_bdt_policy_async(
      body, [this, af_id, bdt_id, bdt_policy,
             sink = std::move(sink)](oai::sba::response r) mutable {
        cont_bdt_create(
            af_id, bdt_id, std::move(bdt_policy), std::move(r),
            std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_bdt_create(
    const std::string& af_id, const std::string& bdt_id,
    oai::_3gpp::model::BdtPolicy bdt_policy, oai::sba::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_bdt_create bdt_id=%s status=%d", bdt_id.c_str(), r.status_code);
  // SUCCESS-ON-3xx: 2xx or 303 is success. A genuine failure rolls back the
  // local state and answers 502.
  if (!sbi_ok_or_303(r)) {
    Logger::nef_app().warn(
        "PCF BDT create failed for bdt_id=%s (http=%d), rolling back local "
        "session",
        bdt_id.c_str(), r.status_code);
    {
      const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
      m_bdt_sessions.erase(bdt_id);
      m_bdt_id2af_id.erase(bdt_id);
      m_bdt_id2pcf_policy_id.erase(bdt_id);
    }
    return sink(
        http_status_code::BAD_GATEWAY,
        make_problem_detail(
            http_status_code::BAD_GATEWAY, "Failed to create BDT policy in PCF")
            .dump());
  }

  // A concurrent AF delete may have removed bdt_id while PCF was in flight. If
  // it is gone, do not resurrect it. The 201 still goes out and reflects the
  // local state we built; the AF delete already cleaned the maps. The PCF BDT
  // policy is left for PCF/AF cleanup — the sync path has no compensating
  // delete either.
  const std::string pcf_bdt_id = nef_async_parse_pcf_bdt_policy_id(r);
  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    if (m_bdt_sessions.find(bdt_id) != m_bdt_sessions.end()) {
      if (!pcf_bdt_id.empty()) m_bdt_id2pcf_policy_id[bdt_id] = pcf_bdt_id;
    } else {
      Logger::nef_app().info(
          "BDT %s vanished during PCF create (concurrent delete); no-op",
          bdt_id.c_str());
    }
  }

  nlohmann::json resp_json;
  to_json(resp_json, bdt_policy);
  resp_json["bdtRefId"] = bdt_id;
  nef_audit::log("CREATE", "BDT", af_id, bdt_id, http_status_code::CREATED);
  sink(http_status_code::CREATED, resp_json.dump());
}

//------------------------------------------------------------------------------
// bdt_policy_update. Authorize, parse, validate, check the owner and resolve
// the PCF id — all unchanged. Only the PCF BDT update becomes an async fire.
//
// FATAL-502: a PCF failure fails the whole request, sbi_error_http_code
// mapping it to 502 (504 on a per-request timeout). On success cont_bdt_update
// overwrites the local store and sends the 200.
void nef_app::bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_af(af_id, NEF_SERVICE_BDT, sink)) return;

  oai::_3gpp::model::BdtPolicy bdt_policy;
  try {
    from_json(body, bdt_policy);
    bdt_policy.validate();
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

  if (!bdt_policy.bdtPolDataIsSet()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Missing required field: bdtPolData")
            .dump());
  }

  std::string pcf_bdt_id;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "BDT policy not found")
              .dump());
    }
    auto owner_it = m_bdt_id2af_id.find(bdt_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN,
              "AF is not allowed to access this resource")
              .dump());
    }
    auto pcf_it = m_bdt_id2pcf_policy_id.find(bdt_id);
    if (pcf_it != m_bdt_id2pcf_policy_id.end()) pcf_bdt_id = pcf_it->second;
  }

  if (pcf_bdt_id.empty()) {
    Logger::nef_app().warn(
        "No PCF BDT policy ID found for BDT session %s", bdt_id.c_str());
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_GATEWAY,
        make_problem_detail(
            http_status_code::BAD_GATEWAY, "Missing PCF BDT policy identifier")
            .dump());
  }
  clear_request_bearer_token();

  // Fire the PCF BDT update.
  m_nef_client->update_pcf_bdt_policy_async(
      pcf_bdt_id, body,
      [this, af_id, bdt_id, bdt_policy,
       sink = std::move(sink)](oai::sba::response r) mutable {
        cont_bdt_update(
            af_id, bdt_id, std::move(bdt_policy), std::move(r),
            std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    oai::_3gpp::model::BdtPolicy bdt_policy, oai::sba::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_bdt_update bdt_id=%s status=%d", bdt_id.c_str(), r.status_code);
  // FATAL-502: a PCF failure fails the whole request (504 on a timeout).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "PCF BDT update failed for bdt_id=%s (http=%d)", bdt_id.c_str(),
        r.status_code);
    const int code = sbi_error_http_code(r);
    return sink(
        code, make_problem_detail(
                  code, "Bad Gateway", "Failed to update BDT policy in PCF")
                  .dump());
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "BDT policy not found")
              .dump());
    }
    session_it->second = bdt_policy;
  }
  nlohmann::json resp_json;
  to_json(resp_json, bdt_policy);
  nef_audit::log("UPDATE", "BDT", af_id, bdt_id, http_status_code::OK);
  sink(http_status_code::OK, resp_json.dump());
}

//------------------------------------------------------------------------------
// bdt_policy_patch. Authorize, check the owner, resolve the PCF id and
// merge-patch into a local copy — all unchanged. Only the PCF BDT update
// becomes an async fire.
//
// FATAL-502: a PCF failure fails the whole request (504 on a timeout). On
// success cont_bdt_patch re-parses, validates, stores and sends the 200.
void nef_app::bdt_patch(
    const std::string& af_id, const std::string& bdt_policy_id,
    const nlohmann::json& patch_body, const std::string& token,
    response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_af(af_id, NEF_SERVICE_BDT, sink)) return;

  std::string pcf_bdt_id;
  nlohmann::json patched_copy;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "BDT policy not found")
              .dump());
    }
    auto owner_it = m_bdt_id2af_id.find(bdt_policy_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN,
              "AF is not allowed to access this resource")
              .dump());
    }
    auto pcf_it = m_bdt_id2pcf_policy_id.find(bdt_policy_id);
    if (pcf_it != m_bdt_id2pcf_policy_id.end()) pcf_bdt_id = pcf_it->second;
    to_json(patched_copy, session_it->second);
    patched_copy.merge_patch(patch_body);
  }
  if (pcf_bdt_id.empty()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_GATEWAY,
        make_problem_detail(
            http_status_code::BAD_GATEWAY, "Missing PCF BDT policy identifier")
            .dump());
  }
  clear_request_bearer_token();

  // Fire the PCF BDT update with the merged copy.
  m_nef_client->update_pcf_bdt_policy_async(
      pcf_bdt_id, patched_copy,
      [this, af_id, bdt_policy_id, patched_copy,
       sink = std::move(sink)](oai::sba::response r) mutable {
        cont_bdt_patch(
            af_id, bdt_policy_id, std::move(patched_copy), std::move(r),
            std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_bdt_patch(
    const std::string& af_id, const std::string& bdt_policy_id,
    nlohmann::json patched_copy, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_bdt_patch bdt_id=%s status=%d", bdt_policy_id.c_str(),
      r.status_code);
  // FATAL-502: a PCF failure fails the whole request (504 on a timeout).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "PCF BDT patch failed for bdt_id=%s (http=%d)", bdt_policy_id.c_str(),
        r.status_code);
    const int code = sbi_error_http_code(r);
    return sink(
        code, make_problem_detail(
                  code, "Bad Gateway", "Failed to update BDT policy in PCF")
                  .dump());
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "BDT policy not found")
              .dump());
    }
    oai::_3gpp::model::BdtPolicy patched_policy;
    try {
      from_json(patched_copy, patched_policy);
      patched_policy.validate();
    } catch (const std::exception& e) {
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY,
              std::string("Patched body invalid: ") + e.what())
              .dump());
    }
    session_it->second = patched_policy;
  }
  patched_copy["bdtRefId"] = bdt_policy_id;
  nef_audit::log("PATCH", "BDT", af_id, bdt_policy_id, http_status_code::OK);
  sink(http_status_code::OK, patched_copy.dump());
}

//------------------------------------------------------------------------------
// bdt_policy_delete. Authorize, check the owner and resolve the PCF id — all
// unchanged. Only the PCF BDT delete becomes an async fire.
//
// BEST-EFFORT: cont_bdt_delete erases the local state and answers 204 whatever
// PCF says.
void nef_app::bdt_delete(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }

  std::string pcf_bdt_id;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    auto owner_it = m_bdt_id2af_id.find(bdt_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(http_status_code::FORBIDDEN, "");
    }
    auto pcf_it = m_bdt_id2pcf_policy_id.find(bdt_id);
    if (pcf_it != m_bdt_id2pcf_policy_id.end()) pcf_bdt_id = pcf_it->second;
  }
  clear_request_bearer_token();

  if (pcf_bdt_id.empty()) {
    // No PCF policy to delete — finish the local cleanup inline (matches the
    // sync skip of the southbound call).
    return cont_bdt_delete(
        af_id, bdt_id, oai::sba::response{}, std::move(sink));
  }
  // Fire the PCF BDT delete
  m_nef_client->delete_pcf_bdt_policy_async(
      pcf_bdt_id, [this, af_id, bdt_id,
                   sink = std::move(sink)](oai::sba::response r) mutable {
        cont_bdt_delete(af_id, bdt_id, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_bdt_delete(
    const std::string& af_id, const std::string& bdt_id, oai::sba::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_bdt_delete bdt_id=%s status=%d", bdt_id.c_str(), r.status_code);
  // Best-effort: PCF result warn-only.
  if (r.status_code != 0 && !sbi_ok(r)) {
    Logger::nef_app().warn(
        "PCF BDT delete failed for bdt_id=%s (http=%d)", bdt_id.c_str(),
        r.status_code);
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    m_bdt_sessions.erase(bdt_id);
    m_bdt_id2af_id.erase(bdt_id);
    m_bdt_id2pcf_policy_id.erase(bdt_id);
  }
  nef_audit::log("DELETE", "BDT", af_id, bdt_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}
