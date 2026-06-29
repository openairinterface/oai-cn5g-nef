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

using namespace oai::nef::app;
using namespace boost::placeholders;
using namespace oai::common::sbi;

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

static nlohmann::json make_problem_detail(
    int status, const std::string& title, const std::string& detail,
    const std::string& instance = "");

static bool validate_nnef_event_exposure_subscription(
    const nlohmann::json& body, std::string& error_detail);

static std::string build_nnef_event_exposure_subscription_path(
    const std::string& subscription_id);

static void finalize_nnef_event_exposure_subscription(
    nlohmann::json& subscription, const std::string& subscription_id);

static bool parse_monitor_expire_time(
    const std::string& value,
    std::chrono::system_clock::time_point& expire_time);

static bool normalize_nnef_pfd_app_data(
    const std::string& app_id, const nlohmann::json& input,
    nlohmann::json& output, std::string& error_detail);

static bool extract_nnef_pfd_transaction_apps(
    const nlohmann::json& body, nlohmann::json& applications,
    std::string& error_detail);

static nlohmann::json make_nnef_pfd_transaction(
    const std::string& transaction_id, const nlohmann::json& body,
    const nlohmann::json& applications);

namespace {
thread_local std::string g_request_bearer_token;
}

// Analytics /fetch endpoint
//------------------------------------------------------------------------------
void nef_app::handle_analytics_fetch(
    const std::string& scs_as_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  if (!body.contains("analyEventsSubs")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing analyEventsSubs in request body");
    return;
  }
  Logger::nef_app().info(
      "Analytics fetch requested by %s: %s", scs_as_id.c_str(),
      body.dump().c_str());

  // Option A: return locally cached analytics from existing subscriptions.
  // Collect subscription data for all ANALYTICS subscriptions owned by this AF
  // whose stored analyEventsSubs overlap the requested events.
  const auto& requested_events = body["analyEventsSubs"];

  nlohmann::json reports = nlohmann::json::array();
  {
    std::shared_lock lock(m_af_subscriptions_mutex);
    for (const auto& [sub_id, sub] : m_af_sub_id2subscription) {
      if (sub->get_service_type() !=
          nef_service_type_t::NEF_SERVICE_TYPE_ANALYTICS)
        continue;
      if (sub->get_scs_as_id() != scs_as_id) continue;
      const nlohmann::json sub_data = sub->get_subscription_data();
      if (!sub_data.contains("analyEventsSubs") ||
          !sub_data["analyEventsSubs"].is_array())
        continue;
      // Check if any requested event type overlaps with the stored subscription
      bool match = false;
      for (const auto& req_ev : requested_events) {
        if (!req_ev.contains("analyEvent")) continue;
        for (const auto& stored_ev : sub_data["analyEventsSubs"]) {
          if (stored_ev.contains("analyEvent") &&
              stored_ev["analyEvent"] == req_ev["analyEvent"]) {
            match = true;
            break;
          }
        }
        if (match) break;
      }
      if (!match) continue;

      nlohmann::json report     = nlohmann::json::object();
      report["subId"]           = sub_id;
      report["analyEventsSubs"] = sub_data["analyEventsSubs"];
      if (sub_data.contains("notifUri"))
        report["notifUri"] = sub_data["notifUri"];
      reports.push_back(report);
    }
  }

  response_body                        = nlohmann::json::object();
  response_body["analyEventsSubs"]     = requested_events;
  response_body["noNetworkSupportInd"] = reports.empty();
  if (!reports.empty()) response_body["analyReports"] = reports;
  http_code = http_status_code::OK;
}

// BDT PATCH
//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_patch(
    const std::string& af_id, const std::string& bdt_policy_id,
    const nlohmann::json& patch_body, nlohmann::json& response_body,
    int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  std::string pcf_bdt_id;
  nlohmann::json patched_copy;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      return;
    }
    auto owner_it = m_bdt_id2af_id.find(bdt_policy_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }
    auto pcf_it = m_bdt_id2pcf_policy_id.find(bdt_policy_id);
    if (pcf_it != m_bdt_id2pcf_policy_id.end()) {
      pcf_bdt_id = pcf_it->second;
    }
    // Serialize typed store to JSON, then apply merge-patch
    to_json(patched_copy, session_it->second);
    patched_copy.merge_patch(patch_body);
  }
  if (pcf_bdt_id.empty()) {
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF BDT policy identifier");
    return;
  }
  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_bdt_policy(
          pcf_bdt_id, patched_copy, http_code_pcf)) {
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update BDT policy in PCF");
    return;
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      return;
    }
    // Re-parse merged JSON back to typed and store
    oai::_3gpp::model::BdtPolicy patched_policy;
    try {
      from_json(patched_copy, patched_policy);
      patched_policy.validate();
    } catch (const std::exception& e) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
          std::string("Patched body invalid: ") + e.what());
      return;
    }
    session_it->second = patched_policy;
  }
  response_body             = patched_copy;
  response_body["bdtRefId"] = bdt_policy_id;
  http_code                 = http_status_code::OK;
  nef_audit::log("PATCH", "BDT", af_id, bdt_policy_id, http_code);
}

// QoS UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Typed parse + validate
  oai::_3gpp::model::AsSessionWithQoSSubscription update_data;
  try {
    from_json(body, update_data);
    update_data.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        std::string("Validation failed: ") + e.what());
    return;
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "QoS subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }
  // Type and length validation (422 for semantic errors).
  {
    const std::string err =
        validate_string_field(body, "notificationDestination", false, 2048);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }
  // SSRF protection: validate callback URI before updating stored state.
  // Per TS 29.122, the callback URI is carried in notificationDestination.
  if (!update_data.getNotificationDestination().empty()) {
    const std::string notif_uri = update_data.getNotificationDestination();
    const std::string uri_err   = validate_callback_uri(notif_uri);
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "notificationDestination: " + uri_err);
      return;
    }
  }
  // Immutability check (TS 29.122 §4.4.13): reject if any immutable field
  // differs from the stored value.
  {
    const nlohmann::json& stored = sub->get_subscription_data();
    for (const char* f :
         {"ueIpv4Addr", "ueIpv6Addr", "macAddr", "ipDomain", "dnn", "snssai",
          "supportedFeatures"}) {
      const bool in_req    = body.contains(f);
      const bool in_stored = stored.contains(f);
      if ((in_req && in_stored && body[f] != stored[f]) ||
          (in_req && !in_stored)) {
        http_code     = http_status_code::BAD_REQUEST;
        response_body = make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Field '") + f + "' is immutable");
        return;
      }
    }
  }

  // No PCF appSessionId means CREATE never completed successfully; reject
  // rather than silently no-op and return 200.
  std::string app_session_id;
  {
    std::shared_lock<std::shared_mutex> l(m_qos_mutex);
    auto it = m_qos_sub_id2pcf_app_session_id.find(sub_id);
    if (it != m_qos_sub_id2pcf_app_session_id.end())
      app_session_id = it->second;
  }
  if (app_session_id.empty() || !is_valid_app_session_id(app_session_id)) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "No active PCF application session for this subscription");
    return;
  }

  sub->set_subscription_data(body);
  if (!update_data.getNotificationDestination().empty()) {
    sub->set_notification_uri(update_data.getNotificationDestination());
  }

  nlohmann::json pcf_patch;
  std::string pcf_err;
  if (build_pcf_qos_body(
          update_data, /*evsubsc_notif_uri=*/"", pcf_patch, pcf_err)) {
    nlohmann::json merge =
        pcf_patch.value("ascReqData", nlohmann::json::object());
    merge.erase("evSubsc");  // subscription persists per §4.15.6.6a
    uint32_t hc = 0;
    if (!m_nef_client->update_pcf_policy_auth(app_session_id, merge, hc)) {
      Logger::nef_app().warn(
          "T8 PUT: PCF update failed for sub=%s (app_session=%s, http=%u); "
          "in-memory state updated, PCF best-effort",
          sub_id.c_str(), app_session_id.c_str(), hc);
    }
  } else {
    Logger::nef_app().warn(
        "T8 PUT: cannot translate PCF body for sub=%s (%s); PCF "
        "subscription left unchanged",
        sub_id.c_str(), pcf_err.c_str());
  }

  response_body = sub->get_subscription_data();
  http_code     = http_status_code::OK;
  nef_audit::log("UPDATE", "QOS", scs_as_id, sub_id, http_code);
}

// Monitoring Event UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "Subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }

  // Parse + validate notificationDestination directly from JSON
  if (!body.contains("notificationDestination") ||
      !body["notificationDestination"].is_string()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: notificationDestination");
    return;
  }
  const std::string notif_dest =
      body["notificationDestination"].get<std::string>();

  // SSRF protection: validate callback URI before updating stored state
  const std::string uri_err = validate_callback_uri(notif_dest);
  if (!uri_err.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notificationDestination: " + uri_err);
    return;
  }

  sub->set_subscription_data(body);
  sub->set_notification_uri(notif_dest);
  if (body.contains("monitorExpireTime") &&
      body["monitorExpireTime"].is_string()) {
    std::chrono::system_clock::time_point expire_time;
    if (parse_monitor_expire_time(
            body["monitorExpireTime"].get<std::string>(), expire_time)) {
      sub->set_expire_time(expire_time);
    }
  }
  // Optionally: re-subscribe to AMF if needed (not implemented here)
  response_body          = sub->get_subscription_data();
  response_body["subId"] = sub_id;
  http_code              = http_status_code::OK;
  nef_audit::log("UPDATE", "ME", scs_as_id, sub_id, http_code);
}

// TI GET
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_get(
    const std::string& af_id, const std::string& app_session_id,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  std::shared_lock lock(m_ti_mutex);
  auto it = m_ti_sessions.find(app_session_id);
  if (it == m_ti_sessions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "TI session not found");
    return;
  }
  auto owner_it = m_ti_id2af_id.find(app_session_id);
  if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
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
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
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

// RFC 7807 Problem Detail helper
static nlohmann::json make_problem_detail(
    int status, const std::string& title, const std::string& detail,
    const std::string& instance) {
  nlohmann::json pd;
  pd["type"]   = "about:blank";
  pd["title"]  = title;
  pd["status"] = status;
  pd["detail"] = detail;
  if (!instance.empty()) pd["instance"] = instance;
  return pd;
}

static bool validate_nnef_event_exposure_subscription(
    const nlohmann::json& body, std::string& error_detail) {
  oai::_3gpp::model::NefEventExposureSubsc subsc;
  try {
    from_json(body, subsc);
    subsc.validate();
  } catch (const nlohmann::json::exception& e) {
    error_detail = std::string("Invalid body: ") + e.what();
    return false;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    error_detail = std::string("Validation failed: ") + e.what();
    return false;
  }

  const auto& events_subs = subsc.getEventsSubs();
  if (events_subs.empty()) {
    error_detail = "eventsSubs is required and must be a non-empty array";
    return false;
  }

  // TS 29.591 §5.4.2: each NefEventSubs item must have a valid event.
  for (std::size_t idx = 0; idx < events_subs.size(); ++idx) {
    if (events_subs[idx].getEvent().getEnumValue() ==
        oai::_3gpp::model::NefEvent_anyOf::eNefEvent_anyOf::
            INVALID_VALUE_OPENAPI_GENERATED) {
      error_detail = "eventsSubs[" + std::to_string(idx) +
                     "].event: required non-empty string";
      return false;
    }
  }

  if (subsc.getNotifUri().empty()) {
    error_detail = "notifUri is required and must be a non-empty string";
    return false;
  }

  if (subsc.getNotifId().empty()) {
    error_detail = "notifId is required and must be a non-empty string";
    return false;
  }

  error_detail = validate_callback_uri(subsc.getNotifUri());
  if (!error_detail.empty()) {
    error_detail = "notifUri: " + error_detail;
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
bool nef_app::is_valid_app_session_id(const std::string& id) {
  if (id.empty() || id.size() > 253) return false;
  if (id.find("..") != std::string::npos) return false;
  for (const unsigned char c : id) {
    if (c == '/' || c == '\\') return false;
    if (std::isspace(c) || std::iscntrl(c)) return false;
  }
  return true;
}

//------------------------------------------------------------------------------
static std::string build_nnef_event_exposure_subscription_path(
    const std::string& subscription_id) {
  return "/nnef-eventexposure/v1/subscriptions/" + subscription_id;
}

//------------------------------------------------------------------------------
static void finalize_nnef_event_exposure_subscription(
    nlohmann::json& subscription, const std::string& subscription_id) {
  subscription["subscriptionId"] = subscription_id;
  subscription["self"] =
      build_nnef_event_exposure_subscription_path(subscription_id);
}

//------------------------------------------------------------------------------
static bool parse_monitor_expire_time(
    const std::string& value,
    std::chrono::system_clock::time_point& expire_time) {
  std::string normalized = value;
  if (normalized.empty()) return false;

  // Accept RFC3339 UTC form and trim fractional seconds/timezone offsets.
  auto dot_pos = normalized.find('.');
  if (dot_pos != std::string::npos) {
    normalized = normalized.substr(0, dot_pos);
  }

  auto plus_pos = normalized.find('+', 19);
  if (plus_pos != std::string::npos) {
    normalized = normalized.substr(0, plus_pos);
  }

  auto minus_pos = normalized.find('-', 19);
  if (minus_pos != std::string::npos) {
    normalized = normalized.substr(0, minus_pos);
  }

  if (!normalized.empty() && normalized.back() == 'Z') {
    normalized.pop_back();
  }

  try {
    auto pt       = boost::posix_time::from_iso_extended_string(normalized);
    std::tm tm    = boost::posix_time::to_tm(pt);
    const auto ts = timegm(&tm);
    if (ts < 0) return false;
    expire_time = std::chrono::system_clock::from_time_t(ts);
    return true;
  } catch (...) {
    return false;
  }
}

//------------------------------------------------------------------------------
static bool normalize_nnef_pfd_app_data(
    const std::string& app_id, const nlohmann::json& input,
    nlohmann::json& output, std::string& error_detail) {
  if (!input.is_object()) {
    error_detail = "Application PFD must be a JSON object";
    return false;
  }

  output = input;
  if (output.contains("externalAppId")) {
    if (!output["externalAppId"].is_string()) {
      error_detail = "externalAppId must be a string";
      return false;
    }
    if (output["externalAppId"].get<std::string>() != app_id) {
      error_detail = "externalAppId must match the target appId";
      return false;
    }
  } else {
    output["externalAppId"] = app_id;
  }

  if (!output.contains("pfds") || !output["pfds"].is_object()) {
    error_detail = "Missing required field: pfds";
    return false;
  }

  for (auto& [pfd_id, pfd_content] : output["pfds"].items()) {
    if (!pfd_content.is_object()) {
      error_detail = "Each PFD entry must be a JSON object";
      return false;
    }

    if (pfd_content.contains("pfdId")) {
      if (!pfd_content["pfdId"].is_string()) {
        error_detail = "pfdId must be a string";
        return false;
      }
      if (pfd_content["pfdId"].get<std::string>() != pfd_id) {
        error_detail = "pfdId must match its map key";
        return false;
      }
    } else {
      pfd_content["pfdId"] = pfd_id;
    }

    if (pfd_content.contains("flowDescriptions") &&
        !pfd_content["flowDescriptions"].is_array()) {
      error_detail = "flowDescriptions must be an array when present";
      return false;
    }
    if (pfd_content.contains("urls") && !pfd_content["urls"].is_array()) {
      error_detail = "urls must be an array when present";
      return false;
    }
    if (pfd_content.contains("domainNames") &&
        !pfd_content["domainNames"].is_array()) {
      error_detail = "domainNames must be an array when present";
      return false;
    }
  }

  return true;
}

//------------------------------------------------------------------------------
static bool extract_nnef_pfd_transaction_apps(
    const nlohmann::json& body, nlohmann::json& applications,
    std::string& error_detail) {
  applications = nlohmann::json::object();

  auto add_app = [&](const std::string& app_id,
                     const nlohmann::json& app_body) -> bool {
    if (app_id.empty()) {
      error_detail = "Application identifier is missing";
      return false;
    }
    nlohmann::json normalized;
    if (!normalize_nnef_pfd_app_data(
            app_id, app_body, normalized, error_detail)) {
      return false;
    }
    applications[app_id] = normalized;
    return true;
  };

  auto add_app_array = [&](const nlohmann::json& app_array) -> bool {
    if (!app_array.is_array()) {
      error_detail =
          "applications must be an array when provided as a collection";
      return false;
    }
    for (const auto& app_body : app_array) {
      if (!app_body.is_object() || !app_body.contains("externalAppId") ||
          !app_body["externalAppId"].is_string()) {
        error_detail = "Each application entry must contain externalAppId";
        return false;
      }
      if (!add_app(app_body["externalAppId"].get<std::string>(), app_body)) {
        return false;
      }
    }
    return true;
  };

  if (body.is_array()) {
    if (!add_app_array(body)) return false;
  } else if (body.is_object()) {
    if (body.contains("applications")) {
      if (body["applications"].is_array()) {
        if (!add_app_array(body["applications"])) return false;
      } else if (body["applications"].is_object()) {
        for (const auto& [app_id, app_body] : body["applications"].items()) {
          if (!add_app(app_id, app_body)) return false;
        }
      } else {
        error_detail = "applications must be an object or an array";
        return false;
      }
    } else if (body.contains("pfdDatas") && body["pfdDatas"].is_object()) {
      for (const auto& [app_id, app_body] : body["pfdDatas"].items()) {
        if (!add_app(app_id, app_body)) return false;
      }
    } else if (
        body.contains("externalAppId") && body["externalAppId"].is_string()) {
      if (!add_app(body["externalAppId"].get<std::string>(), body))
        return false;
    } else {
      error_detail = "Transaction body must contain applications or pfdDatas";
      return false;
    }
  } else {
    error_detail = "Transaction body must be a JSON object or array";
    return false;
  }

  if (applications.empty()) {
    error_detail = "Transaction must contain at least one application PFD";
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
static nlohmann::json make_nnef_pfd_transaction(
    const std::string& transaction_id, const nlohmann::json& body,
    const nlohmann::json& applications) {
  nlohmann::json transaction =
      body.is_object() ? body : nlohmann::json::object();
  transaction["transactionId"] = transaction_id;
  transaction["applications"]  = applications;
  if (transaction.contains("pfdDatas")) {
    transaction.erase("pfdDatas");
  }
  return transaction;
}

// Constructor / Destructor
nef_app::nef_app(const std::string& config_file, nef_event& ev)
    : m_event_sub(ev) {
  Logger::nef_app().startup("Starting NEF application...");

  generate_uuid();

  // Security configuration check — fail-closed by default
  {
    auto nef_cfg               = nef_config_inst->nef();
    const bool whitelist_empty = nef_cfg->get_af_whitelist().empty();
    const bool jwt_empty       = nef_cfg->get_jwt_secret_key().empty();
    if (whitelist_empty && jwt_empty) {
      if (nef_cfg->get_insecure_dev_mode()) {
        Logger::nef_app().error(
            "\n"
            "╔══════════════════════════════════════════════════════════════╗\n"
            "║  SECURITY WARNING: insecure_dev_mode IS ENABLED              ║\n"
            "║  No authentication configured (no JWT secret, no whitelist). ║\n"
            "║  ALL requests will be permitted without any auth check.      ║\n"
            "║  DO NOT USE THIS CONFIGURATION IN PRODUCTION!                ║\n"
            "╚══════════════════════════════════════════════════════════════╝");
      } else {
        Logger::nef_app().info(
            "Auth not configured (no JWT secret, no AF whitelist). "
            "Fail-closed mode: all requests will be DENIED. "
            "Set insecure_dev_mode: true to allow unauthenticated access "
            "(e.g., for testing).");
      }
    }
  }

  m_nef_client = std::make_shared<nef_client>();

  // Bounded thread pool for notification forwarding (4 workers, 1000-task
  // queue).
  // TODO: expose num_threads / max_queue as nef_config parameters.
  m_notification_pool = std::make_unique<notification_thread_pool>(4, 1000);

  subscribe_nf_notification();

  uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

  // Register to NRF
  if (m_nef_client->register_to_nrf()) {
    Logger::nef_app().info("NEF registered to NRF");
    // Periodic NRF heartbeat every 50 s (50000 ms ticks)
    // TODO: get heartbeat interval from NRF response
    constexpr uint64_t HEARTBEAT_MS = 50000;
    auto hb_conn                    = m_event_sub.subscribe_task_tick(
        [this](uint64_t /*t*/) { m_nef_client->send_heartbeat_to_nrf(); },
        HEARTBEAT_MS, now_ms + HEARTBEAT_MS);
    m_connections.push_back(hb_conn);
  } else {
    Logger::nef_app().error("Failed to register NEF to NRF");
    // Exist?
  }

  constexpr uint64_t SUBSCRIPTION_EXPIRY_CHECK_MS = 1000;
  auto expiry_conn = m_event_sub.subscribe_task_tick(
      [this](uint64_t t) { handle_subscription_expiry_tick(t); },
      SUBSCRIPTION_EXPIRY_CHECK_MS, now_ms + SUBSCRIPTION_EXPIRY_CHECK_MS);
  m_connections.push_back(expiry_conn);

  Logger::nef_app().startup("NEF application started");
}

//------------------------------------------------------------------------------
nef_app::~nef_app() {
  Logger::nef_app().debug("Destroying NEF application...");
  for (auto& c : m_connections) {
    if (c.connected()) c.disconnect();
  }
  if (m_notification_pool) {
    m_notification_pool->stop();  // drain queue and join workers
  }
  // Only deregister if not already done explicitly (e.g. by graceful shutdown).
  if (!m_deregistered.exchange(true, std::memory_order_acq_rel)) {
    m_nef_client->deregister_from_nrf();
  }
}

//------------------------------------------------------------------------------
void nef_app::deregister_from_nrf() {
  if (m_deregistered.exchange(true, std::memory_order_acq_rel))
    return;  // double-deregistration guard
  m_nef_client->deregister_from_nrf();
}

// Utility functions
//------------------------------------------------------------------------------
void nef_app::generate_uuid() {
  m_nef_instance_id =
      boost::uuids::to_string(boost::uuids::random_generator()());
  Logger::nef_app().info("NEF instance ID: %s", m_nef_instance_id.c_str());
}

//------------------------------------------------------------------------------
void nef_app::generate_af_subscription_id(std::string& sub_id) {
  uint32_t id = m_sub_id_generator.get_uid();
  std::ostringstream oss;
  oss << std::hex << id;
  sub_id = oss.str();
}

// Authorization
//------------------------------------------------------------------------------
bool nef_app::authorize_af_request(
    const std::string& scs_as_id, const std::string& api_name) const {
  auto nef_cfg          = nef_config_inst->nef();
  const auto& wl        = nef_cfg->get_af_whitelist();
  const auto jwt_secret = nef_cfg->get_jwt_secret_key();

  if (!g_request_bearer_token.empty()) {
    static const nef_jwt jwt_validator;
    if (!jwt_validator.validate_af_token(
            g_request_bearer_token, api_name, scs_as_id)) {
      Logger::nef_app().warn(
          "JWT validation failed for AF %s on API %s", scs_as_id.c_str(),
          api_name.c_str());
      return false;
    }
    // JWT valid: allow immediately — do not fall through to whitelist check
    return true;
  } else if (!jwt_secret.empty()) {
    Logger::nef_app().warn(
        "Missing bearer token for AF %s while jwt_secret is configured",
        scs_as_id.c_str());
    return false;
  }

  // Security configuration check — fail-closed by default
  if (wl.empty() && jwt_secret.empty()) {
    if (nef_cfg->get_insecure_dev_mode()) {
      Logger::nef_app().debug(
          "insecure_dev_mode: allowing %s on %s (no auth configured)",
          scs_as_id.c_str(), api_name.c_str());
      return true;
    } else {
      Logger::nef_app().warn(
          "authorize_af_request(): auth not configured (no JWT secret, no AF "
          "whitelist) – denying %s on %s. Set insecure_dev_mode: true to "
          "allow unauthenticated access (e.g., for testing).",
          scs_as_id.c_str(), api_name.c_str());
      return false;
    }
  }

  // Whitelist configured but empty of matching entries — deny.
  if (wl.empty()) {
    Logger::nef_app().warn(
        "AF %s denied: whitelist is configured but empty", scs_as_id.c_str());
    return false;
  }

  for (const auto& entry : wl) {
    if (entry.af_id != scs_as_id) continue;

    // AF found in whitelist – check allowed APIs
    if (entry.allowed_apis.empty()) {
      Logger::nef_app().debug("AF %s allowed on all APIs", scs_as_id.c_str());
      return true;
    }
    bool allowed = std::find(
                       entry.allowed_apis.begin(), entry.allowed_apis.end(),
                       api_name) != entry.allowed_apis.end();
    if (!allowed) {
      Logger::nef_app().warn(
          "AF %s is NOT allowed to access API %s", scs_as_id.c_str(),
          api_name.c_str());
    }
    return allowed;
  }

  // AF not in whitelist at all
  Logger::nef_app().warn(
      "AF %s is not in the whitelist – denying request", scs_as_id.c_str());
  return false;
}

//------------------------------------------------------------------------------
bool nef_app::authorize_nnef_request(const std::string& api_name) const {
  // For Nnef (SBI) requests the consumer NF identity is in the bearer-token
  // 'sub' claim rather than the URL path.  Extract it and delegate to the
  // standard authorize_af_request() which handles JWT verification, whitelist
  // look-up, and insecure_dev_mode consistently.
  std::string nf_sub;
  if (!g_request_bearer_token.empty()) {
    static const nef_jwt jwt_validator;
    if (!jwt_validator.extract_sub_claim(g_request_bearer_token, nf_sub)) {
      Logger::nef_app().warn(
          "authorize_nnef_request(): malformed bearer token "
          "(cannot extract sub claim) for Nnef service '%s'",
          api_name.c_str());
      return false;
    }
  }
  return authorize_af_request(nf_sub, api_name);
}

//------------------------------------------------------------------------------
void nef_app::set_request_bearer_token(const std::string& bearer_token) const {
  g_request_bearer_token = bearer_token;
}

//------------------------------------------------------------------------------
void nef_app::clear_request_bearer_token() const {
  g_request_bearer_token.clear();
}

//------------------------------------------------------------------------------
std::string nef_app::get_request_bearer_token() const {
  return g_request_bearer_token;
}

// Subscription helpers
//------------------------------------------------------------------------------
bool nef_app::add_subscription(
    const std::string& sub_id, const std::shared_ptr<nef_subscription>& s) {
  std::unique_lock lock(m_af_subscriptions_mutex);
  m_af_sub_id2subscription[sub_id] = s;
  return true;
}

//------------------------------------------------------------------------------
bool nef_app::remove_subscription(const std::string& sub_id) {
  std::unique_lock lock(m_af_subscriptions_mutex);
  auto it = m_af_sub_id2subscription.find(sub_id);
  if (it == m_af_sub_id2subscription.end()) return false;
  m_af_sub_id2subscription.erase(it);
  return true;
}

//------------------------------------------------------------------------------
std::shared_ptr<nef_subscription> nef_app::find_subscription(
    const std::string& sub_id) const {
  std::shared_lock lock(m_af_subscriptions_mutex);
  auto it = m_af_sub_id2subscription.find(sub_id);
  if (it != m_af_sub_id2subscription.end()) return it->second;
  return nullptr;
}

//------------------------------------------------------------------------------
bool nef_app::is_subscription_owner(
    const std::shared_ptr<nef_subscription>& sub,
    const std::string& af_id) const {
  if (!sub) return false;
  const std::string owner = sub->get_scs_as_id();
  if (owner.empty()) {
    Logger::nef_app().warn(
        "Subscription owner missing for AF %s request", af_id.c_str());
    return false;
  }
  if (owner != af_id) {
    Logger::nef_app().warn(
        "AF %s is not owner of subscription (owner=%s)", af_id.c_str(),
        owner.c_str());
    return false;
  }
  return true;
}

// AF Profile helpers
//------------------------------------------------------------------------------
bool nef_app::add_af_profile(
    const std::string& af_id, const std::shared_ptr<nef_af_profile>& p) {
  std::unique_lock lock(m_af_id2profile_mutex);
  m_af_id2profile[af_id] = p;
  Logger::nef_app().debug("AF profile added: %s", af_id.c_str());
  return true;
}

//------------------------------------------------------------------------------
bool nef_app::remove_af_profile(const std::string& af_id) {
  std::unique_lock lock(m_af_id2profile_mutex);
  auto it = m_af_id2profile.find(af_id);
  if (it == m_af_id2profile.end()) return false;
  m_af_id2profile.erase(it);
  Logger::nef_app().debug("AF profile removed: %s", af_id.c_str());
  return true;
}

//------------------------------------------------------------------------------
std::shared_ptr<nef_af_profile> nef_app::find_af_profile(
    const std::string& af_id) const {
  std::shared_lock lock(m_af_id2profile_mutex);
  auto it = m_af_id2profile.find(af_id);
  if (it != m_af_id2profile.end()) return it->second;
  return nullptr;
}

//------------------------------------------------------------------------------
bool nef_app::is_af_registered(const std::string& af_id) const {
  std::shared_lock lock(m_af_id2profile_mutex);
  return m_af_id2profile.count(af_id) > 0;
}

//------------------------------------------------------------------------------
void nef_app::ensure_af_profile(
    const std::string& af_id, const std::string& sub_id) {
  auto profile = find_af_profile(af_id);
  if (!profile) {
    profile = std::make_shared<nef_af_profile>(m_event_sub);
    profile->set_af_id(af_id);
    add_af_profile(af_id, profile);
  }
  profile->add_subscription_id(sub_id);
}

//------------------------------------------------------------------------------
void nef_app::release_af_profile_subscription(
    const std::string& af_id, const std::string& sub_id) {
  auto profile = find_af_profile(af_id);
  if (!profile) return;
  profile->remove_subscription_id(sub_id);
  if (profile->has_no_subscriptions()) {
    remove_af_profile(af_id);
    Logger::nef_app().info(
        "AF %s has no remaining subscriptions – profile destroyed",
        af_id.c_str());
  }
}

// Event subscriptions
//------------------------------------------------------------------------------
void nef_app::subscribe_nf_notification() {
  auto conn = m_event_sub.subscribe_nf_notification(
      boost::bind(&nef_app::handle_nf_notification_event, this, _1, _2));
  m_connections.push_back(conn);
}

//------------------------------------------------------------------------------
void nef_app::handle_nf_notification_event(
    const std::string& nf_sub_id, const nlohmann::json& notif) {
  handle_nf_notification(nf_sub_id, notif);
}

// Inbound notification from 5GC NF
//------------------------------------------------------------------------------
bool nef_app::handle_nf_notification(
    const std::string& nf_sub_id, const nlohmann::json& notif_payload) {
  Logger::nef_app().debug(
      "Received NF notification for NF-sub-id: %s", nf_sub_id.c_str());

  // Map NF sub-id → AF sub-id.
  // PCF QoS callback: PCF appends "/notify" to evSubsc.notifUri, so the
  // extracted key is "{qos_sub_id}/notify".
  std::string af_sub_id;
  {
    std::shared_lock lock(m_nf2af_mutex);
    std::string key = nf_sub_id;
    auto it         = m_nf2af_sub_id.find(key);
    if (it == m_nf2af_sub_id.end()) {
      static const std::string kSfx = "/notify";
      if (key.size() > kSfx.size() &&
          key.compare(key.size() - kSfx.size(), kSfx.size(), kSfx) == 0) {
        key.erase(key.size() - kSfx.size());
        it = m_nf2af_sub_id.find(key);
      }
    }
    if (it == m_nf2af_sub_id.end()) {
      Logger::nef_app().warn(
          "No AF subscription found for NF sub-id: %s", nf_sub_id.c_str());
      return false;
    }
    af_sub_id = it->second;
  }

  // Find the AF subscription
  auto sub = find_subscription(af_sub_id);
  if (!sub) {
    Logger::nef_app().warn("AF subscription %s not found", af_sub_id.c_str());
    return false;
  }

  // Forward to AF — translate southbound → northbound format via mapper
  std::string af_uri = sub->get_notification_uri();
  if (!af_uri.empty()) {
    nlohmann::json t8_payload;
    bool mapped = false;
    auto svc    = sub->get_service_type();
    if (svc == nef_service_type_t::NEF_SERVICE_TYPE_MONITORING_EVENT) {
      mapped = nef_notification_mapper::amf_to_monitoring_notification(
          notif_payload, t8_payload, af_sub_id);
    } else if (svc == nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING) {
      // The UserPlaneNotificationData "transaction" is the AF
      // subscription's self-URI. Fall back to the AF notification URI, then to
      // the bare af_sub_id if the self-URI was not stored.
      std::string transaction = sub->get_self();
      if (transaction.empty()) transaction = af_uri;
      if (transaction.empty()) transaction = af_sub_id;
      mapped = nef_notification_mapper::pcf_to_qos_notification(
          notif_payload, t8_payload, transaction);
    } else if (svc == nef_service_type_t::NEF_SERVICE_TYPE_TRAFFIC_INFLUENCE) {
      mapped = nef_notification_mapper::pcf_to_ti_notification(
          notif_payload, t8_payload, af_sub_id);
    } else {
      // Pass-through for other service types (Analytics, PFD, BDT, etc.)
      t8_payload = notif_payload;
      mapped     = true;
    }

    if (!mapped) {
      Logger::nef_app().warn(
          "Notification mapping failed for sub %s – forwarding raw payload",
          af_sub_id.c_str());
      t8_payload = notif_payload;
    }

    auto nef_client = m_nef_client;
    const bool enqueued =
        m_notification_pool->enqueue([nef_client, af_uri, t8_payload]() {
          if (!nef_client->forward_notification_to_af(af_uri, t8_payload)) {
            Logger::nef_app().warn(
                "Failed forwarding notification to AF endpoint: %s",
                af_uri.c_str());
          }
        });
    if (!enqueued) {
      Logger::nef_app().error(
          "handle_nf_notification: notification queue full (>1000), "
          "dropping notification for AF %s. "
          "Consider increasing thread pool size.",
          af_uri.c_str());
    }
  }
  return true;
}

// Nnef_EventExposure (TS 29.591)
//------------------------------------------------------------------------------
void nef_app::handle_nnef_event_exposure_subscribe(
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }

  std::string error_detail;

  // Typed parse + validate (replaces validate_nnef_event_exposure_subscription)
  oai::_3gpp::model::NefEventExposureSubsc subsc;
  try {
    from_json(body, subsc);
    subsc.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        std::string("Validation failed: ") + e.what());
    return;
  }

  // TS 29.591 §5.4.2: each NefEventSubs must have a valid event
  const auto& events_subs = subsc.getEventsSubs();
  if (events_subs.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "eventsSubs is required and must be a non-empty array");
    return;
  }
  for (std::size_t i = 0; i < events_subs.size(); ++i) {
    if (events_subs[i].getEvent().getEnumValue() ==
        oai::_3gpp::model::NefEvent_anyOf::eNefEvent_anyOf::
            INVALID_VALUE_OPENAPI_GENERATED) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "eventsSubs[" + std::to_string(i) +
              "].event: required non-empty string");
      return;
    }
  }

  if (subsc.getNotifUri().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notifUri is required and must be a non-empty string");
    return;
  }
  if (subsc.getNotifId().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notifId is required and must be a non-empty string");
    return;
  }

  error_detail = validate_callback_uri(subsc.getNotifUri());
  if (!error_detail.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notifUri: " + error_detail);
    return;
  }

  std::string subscription_id;
  generate_af_subscription_id(subscription_id);

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_event_subscriptions_mutex);
    m_nnef_event_subscriptions[subscription_id] = subsc;
  }

  Logger::nef_app().info(
      "Created Nnef_EventExposure subscription: %s", subscription_id.c_str());

  to_json(response_body, subsc);
  response_body["subscriptionId"] = subscription_id;
  response_body["self"] =
      build_nnef_event_exposure_subscription_path(subscription_id);
  http_code = http_status_code::CREATED;
  nef_audit::log("CREATE", "EE", "", subscription_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  const std::lock_guard<std::shared_mutex> lock(
      m_nnef_event_subscriptions_mutex);
  auto it = m_nnef_event_subscriptions.find(subscription_id);
  if (it == m_nnef_event_subscriptions.end()) {
    http_code = http_status_code::NOT_FOUND;
    return;
  }

  m_nnef_event_subscriptions.erase(it);
  Logger::nef_app().info(
      "Deleted Nnef_EventExposure subscription: %s", subscription_id.c_str());
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "EE", "", subscription_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_event_exposure_get(
    const std::string& subscription_id, nlohmann::json& response_body,
    int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }

  std::shared_lock lock(m_nnef_event_subscriptions_mutex);
  auto it = m_nnef_event_subscriptions.find(subscription_id);
  if (it == m_nnef_event_subscriptions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Nnef_EventExposure subscription not found");
    return;
  }

  to_json(response_body, it->second);
  response_body["subscriptionId"] = subscription_id;
  response_body["self"] =
      build_nnef_event_exposure_subscription_path(subscription_id);
  http_code = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_event_exposure_update(
    const std::string& subscription_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }

  std::string error_detail;

  // Typed parse + validate
  oai::_3gpp::model::NefEventExposureSubsc subsc;
  try {
    from_json(body, subsc);
    subsc.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        std::string("Validation failed: ") + e.what());
    return;
  }

  const auto& events_subs = subsc.getEventsSubs();
  if (events_subs.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "eventsSubs is required and must be a non-empty array");
    return;
  }
  for (std::size_t i = 0; i < events_subs.size(); ++i) {
    if (events_subs[i].getEvent().getEnumValue() ==
        oai::_3gpp::model::NefEvent_anyOf::eNefEvent_anyOf::
            INVALID_VALUE_OPENAPI_GENERATED) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "eventsSubs[" + std::to_string(i) +
              "].event: required non-empty string");
      return;
    }
  }

  if (subsc.getNotifUri().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notifUri is required and must be a non-empty string");
    return;
  }
  if (subsc.getNotifId().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notifId is required and must be a non-empty string");
    return;
  }

  error_detail = validate_callback_uri(subsc.getNotifUri());
  if (!error_detail.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notifUri: " + error_detail);
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_event_subscriptions_mutex);
    auto it = m_nnef_event_subscriptions.find(subscription_id);
    if (it == m_nnef_event_subscriptions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "Nnef_EventExposure subscription not found");
      return;
    }

    it->second = subsc;
  }

  Logger::nef_app().info(
      "Updated Nnef_EventExposure subscription: %s", subscription_id.c_str());

  to_json(response_body, subsc);
  response_body["subscriptionId"] = subscription_id;
  response_body["self"] =
      build_nnef_event_exposure_subscription_path(subscription_id);
  http_code = http_status_code::OK;
  nef_audit::log("UPDATE", "EE", "", subscription_id, http_code);
}

// Monitoring Event Exposure (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_create(
    const std::string& scs_as_id, const nlohmann::json& body,
    std::string& sub_id, nlohmann::json& response_body, int& http_code) {
  Logger::nef_app().info(
      "Create monitoring event subscription for SCS/AS: %s", scs_as_id.c_str());

  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Validate required fields
  if (!body.contains("monitoringType") ||
      !body.contains("notificationDestination")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "monitoringType and notificationDestination are required");
    return;
  }

  // Path-parameter, enum, and field validation (422 for semantic errors).
  {
    // Valid MonitoringType values per 3GPP TS 29.122 clause 5.2.4
    static const std::unordered_set<std::string> kMonitoringTypes = {
        "LOSS_OF_CONNECTIVITY",
        "UE_REACHABILITY",
        "LOCATION_REPORTING",
        "CHANGE_OF_IMSI_IMEI_ASSOCIATION",
        "ROAMING_STATUS",
        "COMMUNICATION_FAILURE",
        "AVAILABILITY_AFTER_DDN_FAILURE",
        "NUMBER_OF_UES_IN_AN_AREA",
        "PDN_CONNECTIVITY_STATUS",
        "DOWNLINK_DATA_DELIVERY_STATUS",
        "API_SUPPORT_CAPABILITY",
        "NUM_OF_REGD_UES",
        "NUM_OF_ESTD_PDU_SESSIONS",
        "AREA_OF_INTEREST"};
    std::string err;
    if (err.empty()) err = validate_string_param(scs_as_id, "scsAsId", 256);
    if (err.empty())
      err = validate_enum_field(body, "monitoringType", kMonitoringTypes, true);
    if (err.empty())
      err = validate_string_field(body, "notificationDestination", true, 2048);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate the callback URI before any storage or southbound
  // calls
  {
    const std::string uri_err = validate_callback_uri(
        body["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "notificationDestination: " + uri_err);
      return;
    }
  }

  // Create and store subscription first so callback routing state exists
  // before any southbound side-effects.
  generate_af_subscription_id(sub_id);
  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(sub_id);
  sub->set_scs_as_id(scs_as_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_MONITORING_EVENT);
  sub->set_target_nf_type(nf_type_t::NF_TYPE_AMF);
  sub->set_subscription_data(body);
  if (body.contains("notificationDestination")) {
    sub->set_notification_uri(
        body["notificationDestination"].get<std::string>());
  }
  if (body.contains("monitorExpireTime") &&
      body["monitorExpireTime"].is_string()) {
    std::chrono::system_clock::time_point expire_time;
    if (!parse_monitor_expire_time(
            body["monitorExpireTime"].get<std::string>(), expire_time)) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "Invalid monitorExpireTime format");
      return;
    }
    sub->set_expire_time(expire_time);
  }

  add_subscription(sub_id, sub);
  ensure_af_profile(scs_as_id, sub_id);

  // Subscribe to AMF event-exposure southbound.
  std::string amf_sub_id;
  // TODO: should pass sub_id as well?
  if (!m_nef_client->subscribe_amf_event_exposure(body, amf_sub_id)) {
    Logger::nef_app().warn("Failed to subscribe to AMF event exposure");
    remove_subscription(sub_id);
    release_af_profile_subscription(scs_as_id, sub_id);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to create AMF monitoring subscription");
    return;
  }

  sub->set_nf_subscription_id(amf_sub_id);
  if (!amf_sub_id.empty()) {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[amf_sub_id] = sub_id;
  }

  response_body          = body;
  response_body["subId"] = sub_id;
  http_code              = http_status_code::CREATED;
  nef_audit::log("CREATE", "ME", scs_as_id, sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_delete(
    const std::string& scs_as_id, const std::string& sub_id, int& http_code) {
  Logger::nef_app().info(
      "Delete monitoring event subscription: %s", sub_id.c_str());

  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code = http_status_code::NOT_FOUND;
    return;
  }

  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  // Unsubscribe from AMF
  std::string nf_sub_id = sub->get_nf_subscription_id();
  if (!nf_sub_id.empty()) {
    m_nef_client->unsubscribe_amf_event_exposure(nf_sub_id);
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(nf_sub_id);
  }

  remove_subscription(sub_id);
  release_af_profile_subscription(scs_as_id, sub_id);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "ME", scs_as_id, sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_get(
    const std::string& scs_as_id, const std::string& sub_id,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // GET without sub-id is treated as list operation.
  if (sub_id.empty()) {
    std::shared_lock lock(m_af_subscriptions_mutex);
    response_body = nlohmann::json::array();
    for (const auto& [id, sub] : m_af_sub_id2subscription) {
      if (!sub) continue;
      if (sub->get_service_type() !=
          nef_service_type_t::NEF_SERVICE_TYPE_MONITORING_EVENT) {
        continue;
      }
      if (sub->get_scs_as_id() != scs_as_id) continue;

      nlohmann::json entry = sub->get_subscription_data();
      entry["subId"]       = id;
      response_body.push_back(entry);
    }
    http_code = http_status_code::OK;
    return;
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code = http_status_code::NOT_FOUND;
    return;
  }

  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = http_status_code::OK;
}

// Traffic Influence (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_create(
    const std::string& af_id, const nlohmann::json& body, std::string& ti_id,
    nlohmann::json& response_body, int& http_code) {
  Logger::nef_app().info("Create TI subscription for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Typed parse + validate (ADR-6: raw store and SSRF check preserved below)
  oai::_3gpp::model::TrafficInfluData ti;
  try {
    from_json(body, ti);
    ti.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        std::string("Validation failed: ") + e.what());
    return;
  }

  // Validate required fields from the typed request model.
  if (!ti.afAppIdIsSet() && !ti.trafficFiltersIsSet() &&
      !ti.ethTrafficFiltersIsSet()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
        "required");
    return;
  }

  // Path-parameter and field validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty()) err = validate_string_field(body, "afAppId", false, 256);
    if (err.empty()) err = validate_string_field(body, "dnn", false, 100);
    if (err.empty())
      err = validate_string_field(body, "notificationDestination", false, 2048);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate notification callback URI if provided
  if (body.contains("notificationDestination") &&
      body["notificationDestination"].is_string()) {
    const std::string uri_err = validate_callback_uri(
        body["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "notificationDestination: " + uri_err);
      return;
    }
  }

  generate_af_subscription_id(ti_id);

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    m_ti_sessions[ti_id] = body;
    m_ti_id2af_id[ti_id] = af_id;
  }

  // Register nef_subscription so PCF notifications can be routed back to AF
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

  std::string pcf_policy_id;
  uint32_t http_code_pcf = 0;
  const bool pcf_ok =
      m_nef_client->create_pcf_policy_auth(body, pcf_policy_id, http_code_pcf);
  if (!pcf_ok || http_code_pcf < http_status_code::OK ||
      http_code_pcf >= http_status_code::MULTIPLE_CHOICES) {
    Logger::nef_app().warn(
        "PCF TI create failed for ti_id=%s (http=%u), rolling back local "
        "session",
        ti_id.c_str(), http_code_pcf);
    {
      const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
      m_ti_sessions.erase(ti_id);
      m_ti_id2af_id.erase(ti_id);
      m_ti_id2pcf_policy_id.erase(ti_id);
    }
    remove_subscription(ti_id);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to create policy authorization in PCF");
    return;
  }

  if (pcf_policy_id.empty()) {
    // PCF returned HTTP 2xx but no usable policy ID — treat as failure
    Logger::nef_app().error(
        "handle_traffic_influence_create: PCF returned success but empty "
        "policy ID. "
        "Rolling back TI subscription ti_id='%s'.",
        ti_id.c_str());
    {
      const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
      m_ti_sessions.erase(ti_id);
      m_ti_id2af_id.erase(ti_id);
      m_ti_id2pcf_policy_id.erase(ti_id);
    }
    remove_subscription(ti_id);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF policy identifier in create response");
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    m_ti_id2pcf_policy_id[ti_id] = pcf_policy_id;
  }

  // Wire PCF policy ID → NEF sub ID for notification return path
  ti_sub->set_nf_subscription_id(pcf_policy_id);
  {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[pcf_policy_id] = ti_id;
  }
  Logger::nef_app().debug(
      "TI wired PCF policy '%s' -> NEF sub '%s' for AF notification routing",
      pcf_policy_id.c_str(), ti_id.c_str());

  uint32_t http_code_udr = 0;
  if (!m_nef_client->udr_put_influence_data(ti_id, body, http_code_udr)) {
    Logger::nef_app().warn(
        "UDR influence PUT failed for ti_id=%s (http=%u)", ti_id.c_str(),
        http_code_udr);
  }

  response_body              = body;
  response_body["afTransId"] = ti_id;
  http_code                  = http_status_code::CREATED;
  nef_audit::log("CREATE", "TI", af_id, ti_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_update(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Typed parse + validate (ADR-6: raw store and SSRF check preserved below)
  oai::_3gpp::model::TrafficInfluData ti;
  try {
    from_json(body, ti);
    ti.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        std::string("Validation failed: ") + e.what());
    return;
  }

  // Validate required fields from the typed request model.
  if (!ti.afAppIdIsSet() && !ti.trafficFiltersIsSet() &&
      !ti.ethTrafficFiltersIsSet()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
        "required");
    return;
  }

  // Type and length validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_field(body, "afAppId", false, 256);
    if (err.empty()) err = validate_string_field(body, "dnn", false, 100);
    if (err.empty())
      err = validate_string_field(body, "notificationDestination", false, 2048);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate notification callback URI if provided in update
  if (body.contains("notificationDestination") &&
      body["notificationDestination"].is_string()) {
    const std::string uri_err = validate_callback_uri(
        body["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "notificationDestination: " + uri_err);
      return;
    }
  }

  std::string pcf_policy_id;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      return;
    }

    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }

    auto pcf_it = m_ti_id2pcf_policy_id.find(ti_id);
    if (pcf_it != m_ti_id2pcf_policy_id.end()) {
      pcf_policy_id = pcf_it->second;
    }
  }

  if (pcf_policy_id.empty()) {
    Logger::nef_app().warn(
        "No PCF policy ID found for TI session %s", ti_id.c_str());
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF policy identifier for TI session");
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_policy_auth(
          pcf_policy_id, body, http_code_pcf)) {
    Logger::nef_app().warn(
        "PCF TI update failed for ti_id=%s policy_id=%s (http=%u)",
        ti_id.c_str(), pcf_policy_id.c_str(), http_code_pcf);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update policy authorization in PCF");
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      return;
    }
    session_it->second = body;
  }

  response_body = body;
  http_code     = http_status_code::OK;
  nef_audit::log("UPDATE", "TI", af_id, ti_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_delete(
    const std::string& af_id, const std::string& ti_id, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  std::string pcf_policy_id;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }

    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code = http_status_code::FORBIDDEN;
      return;
    }

    auto pcf_it = m_ti_id2pcf_policy_id.find(ti_id);
    if (pcf_it != m_ti_id2pcf_policy_id.end()) {
      pcf_policy_id = pcf_it->second;
    }
  }

  if (!pcf_policy_id.empty()) {
    uint32_t http_code_pcf = 0;
    if (!m_nef_client->delete_pcf_policy_auth(pcf_policy_id, http_code_pcf)) {
      Logger::nef_app().warn(
          "PCF TI delete failed for ti_id=%s policy_id=%s (http=%u)",
          ti_id.c_str(), pcf_policy_id.c_str(), http_code_pcf);
    }
  }

  uint32_t http_code_udr = 0;
  if (!m_nef_client->udr_delete_influence_data(ti_id, http_code_udr)) {
    Logger::nef_app().warn(
        "UDR influence DELETE failed for ti_id=%s (http=%u)", ti_id.c_str(),
        http_code_udr);
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    m_ti_sessions.erase(ti_id);
    m_ti_id2af_id.erase(ti_id);
    m_ti_id2pcf_policy_id.erase(ti_id);
  }
  // Clean up notification routing state
  if (!pcf_policy_id.empty()) {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(pcf_policy_id);
  }
  remove_subscription(ti_id);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "TI", af_id, ti_id, http_code);
}

// PFD Management (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_pfd_create(
    const std::string& app_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code) {
  Logger::nef_app().info("PFD create for app: %s", app_id.c_str());

  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  if (!body.contains("pfdDatas")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: pfdDatas");
    return;
  }

  // Path-parameter and type validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(app_id, "appId", 256);
    if (err.empty()) err = validate_object_field(body, "pfdDatas", false);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, body)) {
    Logger::nef_app().warn("UDR PFD push failed for app: %s", app_id.c_str());
  }
  response_body = body;
  http_code     = http_status_code::CREATED;
  nef_audit::log("CREATE", "PFD", app_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_delete(const std::string& app_id, int& http_code) {
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  m_nef_client->udr_delete_pfd_data(app_id);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "PFD", app_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_get(
    const std::string& app_id, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  nlohmann::json result;
  uint32_t http_code_udr = 0;
  m_nef_client->udr_get_pfd_data(app_id, result, http_code_udr);

  if (http_code_udr == http_status_code::OK) {
    response_body = result;
    http_code     = http_status_code::OK;
    return;
  }

  if (http_code_udr == http_status_code::NOT_FOUND) {
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD data not found");
    http_code = http_status_code::NOT_FOUND;
    return;
  }

  response_body = make_problem_detail(
      http_status_code::BAD_GATEWAY, "Bad Gateway",
      "UDR returned an unexpected response for PFD GET");
  http_code = http_status_code::BAD_GATEWAY;
}

// BDT Policy (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_create(
    const std::string& af_id, const nlohmann::json& body, std::string& bdt_id,
    nlohmann::json& response_body, int& http_code) {
  Logger::nef_app().info("BDT policy create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Typed parse + validate (replaces manual field checks)
  oai::_3gpp::model::BdtPolicy bdt_policy;
  try {
    from_json(body, bdt_policy);
    bdt_policy.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        std::string("Validation failed: ") + e.what());
    return;
  }

  // T8 NEF API requires bdtPolData; enforce manually (model treats it optional)
  if (!bdt_policy.bdtPolDataIsSet()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: bdtPolData");
    return;
  }

  // Path-parameter validation (422 for semantic errors).
  {
    const std::string err = validate_string_param(af_id, "afId", 256);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  generate_af_subscription_id(bdt_id);

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    m_bdt_sessions[bdt_id] = bdt_policy;
    m_bdt_id2af_id[bdt_id] = af_id;
  }

  std::string pcf_bdt_id;
  uint32_t http_code_pcf = 0;
  const bool pcf_ok =
      m_nef_client->create_pcf_bdt_policy(body, pcf_bdt_id, http_code_pcf);
  if (!pcf_ok || ((http_code_pcf < http_status_code::OK ||
                   http_code_pcf >= http_status_code::MULTIPLE_CHOICES) &&
                  http_code_pcf != http_status_code::SEE_OTHER)) {
    Logger::nef_app().warn(
        "PCF BDT create failed for bdt_id=%s (http=%u), rolling back local "
        "session",
        bdt_id.c_str(), http_code_pcf);
    {
      const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
      m_bdt_sessions.erase(bdt_id);
      m_bdt_id2af_id.erase(bdt_id);
      m_bdt_id2pcf_policy_id.erase(bdt_id);
    }
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to create BDT policy in PCF");
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    m_bdt_id2pcf_policy_id[bdt_id] = pcf_bdt_id;
  }

  nlohmann::json resp_json;
  to_json(resp_json, bdt_policy);
  resp_json["bdtRefId"] = bdt_id;
  response_body         = resp_json;
  http_code             = http_status_code::CREATED;
  nef_audit::log("CREATE", "BDT", af_id, bdt_id, http_code);
}

// BDT Policy (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_update(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Typed parse + validate (replaces manual field checks)
  oai::_3gpp::model::BdtPolicy bdt_policy;
  try {
    from_json(body, bdt_policy);
    bdt_policy.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        std::string("Validation failed: ") + e.what());
    return;
  }

  if (!bdt_policy.bdtPolDataIsSet()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: bdtPolData");
    return;
  }

  std::string pcf_bdt_id;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      return;
    }

    auto owner_it = m_bdt_id2af_id.find(bdt_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }

    auto pcf_it = m_bdt_id2pcf_policy_id.find(bdt_id);
    if (pcf_it != m_bdt_id2pcf_policy_id.end()) {
      pcf_bdt_id = pcf_it->second;
    }
  }

  if (pcf_bdt_id.empty()) {
    Logger::nef_app().warn(
        "No PCF BDT policy ID found for BDT session %s", bdt_id.c_str());
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF BDT policy identifier");
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_bdt_policy(pcf_bdt_id, body, http_code_pcf)) {
    Logger::nef_app().warn(
        "PCF BDT update failed for bdt_id=%s policy_id=%s (http=%u)",
        bdt_id.c_str(), pcf_bdt_id.c_str(), http_code_pcf);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update BDT policy in PCF");
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      return;
    }
    session_it->second = bdt_policy;
  }

  nlohmann::json resp_json;
  to_json(resp_json, bdt_policy);
  response_body = resp_json;
  http_code     = http_status_code::OK;
  nef_audit::log("UPDATE", "BDT", af_id, bdt_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_delete(
    const std::string& af_id, const std::string& bdt_id, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  std::string pcf_bdt_id;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }

    auto owner_it = m_bdt_id2af_id.find(bdt_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code = http_status_code::FORBIDDEN;
      return;
    }

    auto pcf_it = m_bdt_id2pcf_policy_id.find(bdt_id);
    if (pcf_it != m_bdt_id2pcf_policy_id.end()) {
      pcf_bdt_id = pcf_it->second;
    }
  }

  if (!pcf_bdt_id.empty()) {
    uint32_t http_code_pcf = 0;
    if (!m_nef_client->delete_pcf_bdt_policy(pcf_bdt_id, http_code_pcf)) {
      Logger::nef_app().warn(
          "PCF BDT delete failed for bdt_id=%s policy_id=%s (http=%u)",
          bdt_id.c_str(), pcf_bdt_id.c_str(), http_code_pcf);
    }
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    m_bdt_sessions.erase(bdt_id);
    m_bdt_id2af_id.erase(bdt_id);
    m_bdt_id2pcf_policy_id.erase(bdt_id);
  }
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "BDT", af_id, bdt_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
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
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  std::shared_lock lock(m_bdt_mutex);
  auto it = m_bdt_sessions.find(bdt_id);
  if (it == m_bdt_sessions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
    return;
  }

  auto owner_it = m_bdt_id2af_id.find(bdt_id);
  if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this resource");
    return;
  }

  to_json(response_body, it->second);
  response_body["bdtRefId"] = bdt_id;
  http_code                 = http_status_code::OK;
}

// QoS Provisioning (TS 29.122) API handlers
//------------------------------------------------------------------------------
// Translate a T8 AsSessionWithQoSSubscription (TS 29.122) into a PCF
// AppSessionContext{ascReqData} JSON body (TS 29.514).
bool nef_app::build_pcf_qos_body(
    const oai::_3gpp::model::AsSessionWithQoSSubscription& req,
    const std::string& evsubsc_notif_uri, nlohmann::json& pcf_body,
    std::string& err) {
  err.clear();
  nlohmann::json asc = nlohmann::json::object();

  // --- UE addressing (oneOf) -------------------------------------------------
  if (req.ueIpv4AddrIsSet()) asc["ueIpv4"] = req.getUeIpv4Addr();
  if (req.ueIpv6AddrIsSet()) asc["ueIpv6"] = req.getUeIpv6Addr();
  if (req.macAddrIsSet()) asc["ueMac"] = req.getMacAddr();
  if (req.ipDomainIsSet()) asc["ipDomain"] = req.getIpDomain();

  // --- Feature negotiation (required by PCF; default "0" when absent) --------
  asc["suppFeat"] = req.supportedFeaturesIsSet() ? req.getSupportedFeatures() :
                                                   std::string("0");

  // --- Targeting -------------------------------------------------------------
  if (req.dnnIsSet()) asc["dnn"] = req.getDnn();
  if (req.snssaiIsSet()) {
    nlohmann::json snssai_j;
    to_json(snssai_j, req.getSnssai());
    asc["sliceInfo"] = snssai_j;
  }
  if (req.exterAppIdIsSet()) asc["afAppId"] = req.getExterAppId();
  if (req.sponsorInfoIsSet()) {
    const auto sponsor = req.getSponsorInfo();
    asc["aspId"]       = sponsor.getAspId();
    asc["sponId"]      = sponsor.getSponsorId();
  }

  // --- NEF inbound endpoint (NOT the AF notificationDestination) -------------
  // PCF callback template is "{evSubsc/notifUri}/notify"; set both fields.
  if (!evsubsc_notif_uri.empty()) asc["notifUri"] = evsubsc_notif_uri;

  // --- Media components from flowInfo[]/qosReference -------------------------
  nlohmann::json med_components = nlohmann::json::object();
  if (req.flowInfoIsSet() && !req.getFlowInfo().empty()) {
    for (const auto& fi : req.getFlowInfo()) {
      // FlowId is int32_t in the typed model; safe to to_string as a key.
      const int32_t flow_id = fi.getFlowId();
      const std::string key = std::to_string(flow_id);
      nlohmann::json mc     = nlohmann::json::object();
      mc["medCompN"]        = flow_id;
      if (req.qosReferenceIsSet()) mc["qosReference"] = req.getQosReference();
      if (req.altQoSReferencesIsSet())
        mc["altSerReqs"] = req.getAltQoSReferences();
      if (fi.flowDescriptionsIsSet()) {
        nlohmann::json sub_comp = nlohmann::json::object();
        sub_comp["fDescs"]      = fi.getFlowDescriptions();
        mc["medSubComps"][key]  = sub_comp;
      }
      med_components[key] = mc;
    }
  } else if (req.qosReferenceIsSet()) {
    // No flows but a QoS reference present: emit a single default component.
    nlohmann::json mc  = nlohmann::json::object();
    mc["medCompN"]     = 0;
    mc["qosReference"] = req.getQosReference();
    if (req.altQoSReferencesIsSet())
      mc["altSerReqs"] = req.getAltQoSReferences();
    med_components["0"] = mc;
  }
  if (!med_components.empty()) asc["medComponents"] = med_components;

  // --- Event subscription block -------------------------------------
  // Only when an inbound NEF endpoint is supplied (CREATE/PUT, not PATCH).
  if (!evsubsc_notif_uri.empty()) {
    nlohmann::json ev_subsc = nlohmann::json::object();
    ev_subsc["notifUri"]    = evsubsc_notif_uri;

    // Translate requested T8 UserPlaneEvent -> PCF AfEvent, de-duplicating.
    // SUCCESSFUL_/FAILED_RESOURCES_ALLOCATION are always present (step-6
    // SHALL).
    std::set<std::string> af_events;
    af_events.insert("SUCCESSFUL_RESOURCES_ALLOCATION");
    af_events.insert("FAILED_RESOURCES_ALLOCATION");

    if (req.eventsIsSet()) {
      for (const auto& ev : req.getEvents()) {
        nlohmann::json ev_j;
        to_json(ev_j, ev);
        const std::string ev_str =
            ev_j.is_string() ? ev_j.get<std::string>() : "";
        if (ev_str == "SUCCESSFUL_RESOURCES_ALLOCATION" ||
            ev_str == "FAILED_RESOURCES_ALLOCATION") {
          // already injected
        } else if (
            ev_str == "QOS_GUARANTEED" || ev_str == "QOS_NOT_GUARANTEED") {
          af_events.insert("QOS_NOTIF");
        } else if (ev_str == "QOS_MONITORING") {
          af_events.insert("QOS_MONITORING");
        } else if (ev_str == "USAGE_REPORT") {
          af_events.insert("USAGE_REPORT");
        } else if (ev_str == "ACCESS_TYPE_CHANGE") {
          af_events.insert("ACCESS_TYPE_CHANGE");
        } else if (ev_str == "PLMN_CHG") {
          af_events.insert("PLMN_CHG");
        } else if (
            ev_str == "SESSION_TERMINATION" || ev_str == "RELEASE_OF_BEARER") {
          // PCF 'terminate' callback handles these; no AfEvent. Skip.
          Logger::nef_app().debug(
              "PCF: T8 event '%s' has no AfEvent analogue — skipped",
              ev_str.c_str());
        } else {
          Logger::nef_app().debug(
              "PCF: unrecognised T8 event '%s' — skipped", ev_str.c_str());
        }
      }
    } else if (req.qosMonInfoIsSet()) {
      // Default-event rule: no explicit events but QoS monitoring requested.
      af_events.insert("QOS_MONITORING");
    }

    nlohmann::json events_arr = nlohmann::json::array();
    for (const auto& e : af_events) {
      events_arr.push_back(nlohmann::json{{"event", e}});
    }
    ev_subsc["events"] = events_arr;

    if (req.qosMonInfoIsSet()) {
      nlohmann::json qos_mon_j;
      to_json(qos_mon_j, req.getQosMonInfo());
      ev_subsc["qosMon"] = qos_mon_j;
    }
    if (req.usageThresholdIsSet()) {
      nlohmann::json usg_j;
      to_json(usg_j, req.getUsageThreshold());
      ev_subsc["usgThres"] = usg_j;
    }
    if (req.directNotifIndIsSet()) {
      asc["directNotifInd"]      = req.isDirectNotifInd();
      ev_subsc["directNotifInd"] = req.isDirectNotifInd();
    }

    asc["evSubsc"] = ev_subsc;
  }

  pcf_body = nlohmann::json{{"ascReqData", asc}};
  return true;
}

//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_create(
    const std::string& af_id, const nlohmann::json& body,
    std::string& qos_sub_id, nlohmann::json& response_body, int& http_code) {
  Logger::nef_app().info("QoS subscription create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Typed parse + validate
  oai::_3gpp::model::AsSessionWithQoSSubscription req_data = {};
  try {
    from_json(body, req_data);
    req_data.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        std::string("Validation failed: ") + e.what());
    return;
  }

  // Validate required fields from the typed request model.
  // Per TS 29.122 (AsSessionWithQoSSubscription), notificationDestination is
  // the only mandatory field.
  if (req_data.getNotificationDestination().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notificationDestination is required");
    return;
  }

  // Path-parameter and field validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty())
      err = validate_string_field(body, "notificationDestination", true, 2048);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate the callback URI before any storage or southbound
  // calls
  {
    const std::string uri_err =
        validate_callback_uri(req_data.getNotificationDestination());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "notificationDestination: " + uri_err);
      return;
    }
  }

  generate_af_subscription_id(qos_sub_id);
  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(qos_sub_id);
  sub->set_scs_as_id(af_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING);
  sub->set_target_nf_type(nf_type_t::NF_TYPE_PCF);
  sub->set_subscription_data(body);
  // TODO: verify Request Expiry

  add_subscription(qos_sub_id, sub);
  ensure_af_profile(af_id, qos_sub_id);

  // evSubsc.notifUri is the NEF inbound endpoint keyed by qos_sub_id.
  // PCF callback template (TS 29.514) appends "/notify", so inbound
  // notifications arrive at ".../notify/{qos_sub_id}/notify"; the scoped
  // fallback in handle_nf_notification strips that suffix.
  const std::string evsubsc_notif_uri =
      nef_config_inst->get_local()->get_url() +
      oai::nef::api::nef_sbi_helper::NefNotifyBase +
      nef_config_inst->nef()->get_sbi().get_api_version() + "/notify/" +
      qos_sub_id;

  nlohmann::json pcf_body;
  std::string pcf_translate_err;
  if (!build_pcf_qos_body(
          req_data, evsubsc_notif_uri, pcf_body, pcf_translate_err)) {
    remove_subscription(qos_sub_id);
    release_af_profile_subscription(af_id, qos_sub_id);
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", pcf_translate_err);
    return;
  }

  std::string pcf_app_session_id;
  uint32_t http_code_pcf = 0;
  const bool pcf_ok      = m_nef_client->create_pcf_policy_auth(
      pcf_body, pcf_app_session_id, http_code_pcf);

  // PCF must succeed AND return a valid appSessionId before storing.
  if (!pcf_ok || !is_valid_app_session_id(pcf_app_session_id)) {
    remove_subscription(qos_sub_id);
    release_af_profile_subscription(af_id, qos_sub_id);
    http_code = http_status_code::INTERNAL_SERVER_ERROR;  // TS 29.522 §4.4.9
    response_body = make_problem_detail(
        http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
        "Failed to create policy authorization in PCF");
    return;
  }

  sub->set_nf_subscription_id(pcf_app_session_id);
  {
    const std::lock_guard<std::shared_mutex> lock(m_qos_mutex);
    m_qos_sub_id2pcf_app_session_id[qos_sub_id] = pcf_app_session_id;
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[qos_sub_id] = qos_sub_id;  // primary (path key)
    m_nf2af_sub_id[pcf_app_session_id] =
        qos_sub_id;  // secondary (appSessionId)
  }
  if (!req_data.getNotificationDestination().empty()) {
    sub->set_notification_uri(req_data.getNotificationDestination());
  }

  // Build the resource self-URI and return the full
  // AsSessionWithQoSSubscription as the response body (per TS 29.122). The
  // self-URI is a relative path; the HTTP layer prefixes it with the server
  // address for the Location header.
  const std::string self_uri =
      oai::nef::api::nef_sbi_helper::NefQosMonitoringBase +
      nef_config_inst->nef()->get_sbi().get_api_version() + "/" + af_id + "/" +
      oai::nef::api::nef_sbi_helper::NefResourceSubscriptions + "/" +
      qos_sub_id;
  req_data.setSelf(self_uri);
  // Persist the self-URI on the subscription so the inbound QoS notification
  // path can use it as the UserPlaneNotificationData "transaction" reference.
  sub->set_self(self_uri);
  to_json(response_body, req_data);
  http_code = http_status_code::CREATED;
  nef_audit::log("CREATE", "QOS", af_id, qos_sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_delete(
    const std::string& af_id, const std::string& qos_sub_id, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  auto sub = find_subscription(qos_sub_id);
  if (!sub) {
    http_code = http_status_code::NOT_FOUND;
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  std::string nf_sub_id = sub->get_nf_subscription_id();
  if (!nf_sub_id.empty()) {
    m_nef_client->unsubscribe_smf_event_exposure(nf_sub_id);
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(qos_sub_id);  // keyed by notifId == qos_sub_id (T5/T9)
  }

  remove_subscription(qos_sub_id);
  release_af_profile_subscription(af_id, qos_sub_id);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "QOS", af_id, qos_sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_get(
    const std::string& af_id, const std::string& qos_sub_id,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  auto sub = find_subscription(qos_sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "QoS subscription not found");
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  std::shared_lock lock(m_af_subscriptions_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [id, sub] : m_af_sub_id2subscription) {
    if (sub->get_service_type() ==
            nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING &&
        sub->get_scs_as_id() == af_id) {
      nlohmann::json entry = sub->get_subscription_data();
      entry["subId"]       = id;
      response_body.push_back(entry);
    }
  }
  http_code = http_status_code::OK;
}

// Analytics Subscription (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_create(
    const std::string& af_id, const nlohmann::json& body,
    std::string& analytics_sub_id, nlohmann::json& response_body,
    int& http_code) {
  Logger::nef_app().info(
      "Analytics subscription create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  if (!body.contains("analyEventsSubs") || !body.contains("notifUri") ||
      !body.contains("notifId")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "analyEventsSubs, notifUri, and notifId are required");
    return;
  }

  // Path-parameter and field validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty()) err = validate_string_field(body, "notifUri", true, 2048);
    if (err.empty()) err = validate_string_field(body, "notifId", true, 256);
    if (err.empty())
      err = validate_array_field(body, "analyEventsSubs", false, 1);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate the callback URI before any storage
  {
    const std::string uri_err =
        validate_callback_uri(body["notifUri"].get<std::string>());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
      return;
    }
  }

  generate_af_subscription_id(analytics_sub_id);
  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(analytics_sub_id);
  sub->set_scs_as_id(af_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_ANALYTICS);
  sub->set_subscription_data(body);

  add_subscription(analytics_sub_id, sub);
  ensure_af_profile(af_id, analytics_sub_id);
  response_body          = body;
  response_body["subId"] = analytics_sub_id;
  http_code              = http_status_code::CREATED;
  nef_audit::log("CREATE", "ANA", af_id, analytics_sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_delete(
    const std::string& af_id, const std::string& analytics_sub_id,
    int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  auto sub = find_subscription(analytics_sub_id);
  if (!sub) {
    http_code = http_status_code::NOT_FOUND;
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  if (!remove_subscription(analytics_sub_id)) {
    http_code = http_status_code::NOT_FOUND;
    return;
  }
  release_af_profile_subscription(af_id, analytics_sub_id);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "ANA", af_id, analytics_sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_get(
    const std::string& af_id, const std::string& analytics_sub_id,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  auto sub = find_subscription(analytics_sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Analytics subscription not found");
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  std::shared_lock lock(m_af_subscriptions_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [id, sub] : m_af_sub_id2subscription) {
    if (sub->get_service_type() ==
            nef_service_type_t::NEF_SERVICE_TYPE_ANALYTICS &&
        sub->get_scs_as_id() == af_id) {
      nlohmann::json entry = sub->get_subscription_data();
      entry["subId"]       = id;
      response_body.push_back(entry);
    }
  }
  http_code = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_subscription_expiry_tick(uint64_t t) {
  (void) t;
  const auto now = std::chrono::system_clock::now();

  // Expire Nnef_EventExposure (SBI) subscriptions via eventsRepInfo.monDur
  {
    std::vector<std::string> nnef_expired;
    {
      std::shared_lock lock(m_nnef_event_subscriptions_mutex);
      for (const auto& [sub_id, sub] : m_nnef_event_subscriptions) {
        if (!sub.eventsRepInfoIsSet()) continue;
        const auto& rep = sub.getEventsRepInfo();
        if (!rep.monDurIsSet()) continue;
        std::chrono::system_clock::time_point expire_tp;
        if (!parse_monitor_expire_time(rep.getMonDur(), expire_tp)) continue;
        if (expire_tp <= now) nnef_expired.push_back(sub_id);
      }
    }
    if (!nnef_expired.empty()) {
      const std::lock_guard<std::shared_mutex> lock(
          m_nnef_event_subscriptions_mutex);
      for (const auto& sub_id : nnef_expired) {
        m_nnef_event_subscriptions.erase(sub_id);
        Logger::nef_app().info(
            "Nnef_EventExposure subscription %s expired - removing",
            sub_id.c_str());
      }
    }
  }

  std::vector<std::string> expired_sub_ids;
  {
    std::shared_lock lock(m_af_subscriptions_mutex);
    for (const auto& [sub_id, sub] : m_af_sub_id2subscription) {
      if (!sub || !sub->has_expire_time()) continue;
      if (sub->get_expire_time() <= now) {
        expired_sub_ids.push_back(sub_id);
      }
    }
  }

  for (const auto& sub_id : expired_sub_ids) {
    auto sub = find_subscription(sub_id);
    if (!sub || !sub->has_expire_time()) continue;
    if (sub->get_expire_time() > now) continue;

    const std::string nf_sub_id = sub->get_nf_subscription_id();
    const auto nf_type          = sub->get_target_nf_type();
    const auto svc_type         = sub->get_service_type();

    // Service-specific southbound cleanup before removing local state.
    // TI subscriptions store the PCF policy ID in nf_sub_id and must call
    // delete_pcf_policy_auth rather than the NF event-exposure unsubscribe
    // paths.
    if (svc_type == nef_service_type_t::NEF_SERVICE_TYPE_TRAFFIC_INFLUENCE) {
      if (!nf_sub_id.empty()) {
        uint32_t http_code_pcf = 0;
        if (!m_nef_client->delete_pcf_policy_auth(nf_sub_id, http_code_pcf)) {
          Logger::nef_app().warn(
              "F1.3: PCF TI expiry delete failed for sub=%s (http=%u)",
              sub_id.c_str(), http_code_pcf);
        }
        uint32_t http_code_udr = 0;
        if (!m_nef_client->udr_delete_influence_data(sub_id, http_code_udr)) {
          Logger::nef_app().warn(
              "F1.3: UDR TI expiry delete failed for sub=%s (http=%u)",
              sub_id.c_str(), http_code_udr);
        }
        {
          const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
          m_nf2af_sub_id.erase(nf_sub_id);
        }
      }
      {
        const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
        m_ti_sessions.erase(sub_id);
        m_ti_id2af_id.erase(sub_id);
        m_ti_id2pcf_policy_id.erase(sub_id);
      }
    } else {
      // Monitoring and QoS subscriptions: unsubscribe from the target NF.
      if (!nf_sub_id.empty()) {
        if (nf_type == nf_type_t::NF_TYPE_AMF) {
          m_nef_client->unsubscribe_amf_event_exposure(nf_sub_id);
        } else if (nf_type == nf_type_t::NF_TYPE_SMF) {
          m_nef_client->unsubscribe_smf_event_exposure(nf_sub_id);
        }
        {
          const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
          m_nf2af_sub_id.erase(nf_sub_id);
        }
      }
    }

    remove_subscription(sub_id);
    release_af_profile_subscription(sub->get_scs_as_id(), sub_id);
    Logger::nef_app().info(
        "Subscription %s expired - cleaning up", sub_id.c_str());
  }
}

// TI PATCH handler (TS 29.122)
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_patch(
    const std::string& af_id, const std::string& app_session_id,
    const nlohmann::json& patch_body, nlohmann::json& response_body,
    int& http_code) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Typed parse + validate patch body (ADR-6: raw store preserved below)
  oai::_3gpp::model::TrafficInfluDataPatch ti_patch;
  try {
    from_json(patch_body, ti_patch);
    ti_patch.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        std::string("Validation failed: ") + e.what());
    return;
  }

  std::string pcf_policy_id;
  nlohmann::json patched_copy;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(app_session_id);
    if (session_it == m_ti_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      return;
    }
    auto owner_it = m_ti_id2af_id.find(app_session_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }
    auto pcf_it = m_ti_id2pcf_policy_id.find(app_session_id);
    if (pcf_it != m_ti_id2pcf_policy_id.end()) {
      pcf_policy_id = pcf_it->second;
    }
    patched_copy = session_it->second;
    patched_copy.merge_patch(patch_body);
  }

  if (pcf_policy_id.empty()) {
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF policy identifier for TI session");
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_policy_auth(
          pcf_policy_id, patched_copy, http_code_pcf)) {
    Logger::nef_app().warn(
        "PCF TI patch failed for ti_id=%s (http=%u)", app_session_id.c_str(),
        http_code_pcf);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update policy authorization in PCF");
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(app_session_id);
    if (session_it == m_ti_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      return;
    }
    session_it->second = patched_copy;
  }

  response_body              = patched_copy;
  response_body["afTransId"] = app_session_id;
  http_code                  = http_status_code::OK;
  nef_audit::log("PATCH", "TI", af_id, app_session_id, http_code);
}

// QoS PATCH handler (TS 29.122)
//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_patch(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& patch_body, nlohmann::json& response_body,
    int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "QoS subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }

  // T8 (P8): this resource is a T8 AsSessionWithQoSSubscription, not the PCF
  // AppSessionContext. There is no typed *Patch model (deferred per plan §7
  // Option 2), so apply an RFC 7396 JSON merge-patch to the stored body and
  // re-validate the merged result against the full T8 schema. A merge-patch
  // value of null deletes that key (e.g. {"qosMonInfo": null} removes
  // qosMonInfo); nulling a required field (notificationDestination) makes the
  // merged object fail validation and is rejected with 400 rather than
  // corrupting stored state.
  nlohmann::json patched = sub->get_subscription_data();
  patched.merge_patch(patch_body);

  oai::_3gpp::model::AsSessionWithQoSSubscription merged_data;
  try {
    from_json(patched, merged_data);
    merged_data.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Invalid patched body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    // Merged object no longer satisfies the T8 schema (e.g. a required field
    // was nulled). Reject with 400 and leave stored state untouched.
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        std::string("Patch produces an invalid subscription: ") + e.what());
    return;
  }
  if (merged_data.getNotificationDestination().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notificationDestination is required and cannot be removed");
    return;
  }

  // SSRF protection: validate the (possibly changed) callback URI before
  // persisting or propagating.
  {
    const std::string uri_err =
        validate_callback_uri(merged_data.getNotificationDestination());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "notificationDestination: " + uri_err);
      return;
    }
  }

  // Immutability check (TS 29.122 §4.4.13): the patched object must not
  // change immutable fields relative to the stored original.
  {
    const nlohmann::json& stored = sub->get_subscription_data();
    for (const char* f :
         {"ueIpv4Addr", "ueIpv6Addr", "macAddr", "ipDomain", "dnn", "snssai",
          "supportedFeatures"}) {
      const bool in_patch  = patched.contains(f);
      const bool in_stored = stored.contains(f);
      if ((in_patch && in_stored && patched[f] != stored[f]) ||
          (in_patch && !in_stored)) {
        http_code     = http_status_code::BAD_REQUEST;
        response_body = make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Field '") + f + "' is immutable");
        return;
      }
    }
  }

  // Update-before-Create guard
  std::string app_session_id;
  {
    std::shared_lock<std::shared_mutex> l(m_qos_mutex);
    auto it = m_qos_sub_id2pcf_app_session_id.find(sub_id);
    if (it != m_qos_sub_id2pcf_app_session_id.end())
      app_session_id = it->second;
  }
  if (app_session_id.empty() || !is_valid_app_session_id(app_session_id)) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "No active PCF application session for this subscription");
    return;
  }

  sub->set_subscription_data(patched);
  sub->set_notification_uri(merged_data.getNotificationDestination());

  nlohmann::json pcf_patch;
  std::string pcf_err;
  if (build_pcf_qos_body(
          merged_data, /*evsubsc_notif_uri=*/"", pcf_patch, pcf_err)) {
    nlohmann::json merge =
        pcf_patch.value("ascReqData", nlohmann::json::object());
    merge.erase("evSubsc");  // subscription persists per §4.15.6.6a
    uint32_t hc = 0;
    if (!m_nef_client->update_pcf_policy_auth(app_session_id, merge, hc)) {
      Logger::nef_app().warn(
          "T8 PATCH: PCF update failed for sub=%s (app_session=%s, http=%u); "
          "in-memory state updated",
          sub_id.c_str(), app_session_id.c_str(), hc);
    }
  } else {
    Logger::nef_app().warn(
        "T8 PATCH: cannot translate PCF body for sub=%s (%s); PCF "
        "subscription left unchanged",
        sub_id.c_str(), pcf_err.c_str());
  }

  response_body = patched;
  http_code     = http_status_code::OK;
  nef_audit::log("PATCH", "QOS", scs_as_id, sub_id, http_code);
}

// PFD transaction-level and app-level endpoints (TS 29.122)
//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_list(
    const std::string& scs_as_id, nlohmann::json& response_body,
    int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  std::shared_lock lock(m_pfd_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [tid, app_map] : m_pfd_trans_sessions) {
    auto owner_it = m_pfd_trans2scs_id.find(tid);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id)
      continue;
    nlohmann::json entry;
    entry["transId"]  = tid;
    entry["pfdDatas"] = nlohmann::json::object();
    for (const auto& [app_id, app] : app_map) {
      nlohmann::json app_json;
      to_json(app_json, app);
      entry["pfdDatas"][app_id] = app_json;
    }
    response_body.push_back(entry);
  }
  http_code = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  if (!body.contains("pfdDatas")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: pfdDatas");
    return;
  }

  // Type validation (422 for semantic errors).
  if (!body["pfdDatas"].is_object()) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        "pfdDatas: must be an object");
    return;
  }

  // Typed parse + validate each app's PFD data
  std::map<std::string, oai::_3gpp::model::PfdDataForApp> app_map;
  for (auto& [app_id, app_json] : body["pfdDatas"].items()) {
    oai::_3gpp::model::PfdDataForApp app;
    try {
      from_json(app_json, app);
      app.validate();
    } catch (const nlohmann::json::exception& e) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "pfdDatas." + app_id + ": " + e.what());
      return;
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
          "pfdDatas." + app_id + ": " + e.what());
      return;
    }
    app_map[app_id] = std::move(app);
  }

  // Determine create-vs-update before UDR writes (deferred local commit)
  bool is_create;
  {
    std::shared_lock lock(m_pfd_mutex);
    is_create =
        (m_pfd_trans_sessions.find(trans_id) == m_pfd_trans_sessions.end());
  }

  // Write each app to UDR atomically — rollback committed apps on failure
  PfdRollbackTracker pfd_rollback;
  for (auto& [app_id, app] : app_map) {
    nlohmann::json pfd_json;
    to_json(pfd_json, app);
    if (!m_nef_client->udr_put_pfd_data(app_id, pfd_json)) {
      Logger::nef_app().error(
          "F1.10: UDR PFD write failed for app '%s' in trans '%s'; "
          "rolling back %zu committed app(s)",
          app_id.c_str(), trans_id.c_str(), pfd_rollback.committed_count());
      const int rb_failures = pfd_rollback.execute(
          [this](const std::string& rid) {
            return m_nef_client->udr_delete_pfd_data(rid);
          },
          [&trans_id](const std::string& rid) {
            Logger::nef_app().error(
                "F1.10: Rollback delete failed for app '%s' in trans '%s'",
                rid.c_str(), trans_id.c_str());
          });
      if (rb_failures > 0) {
        Logger::nef_app().error(
            "F1.10: %d rollback failure(s) in trans '%s' — UDR may retain "
            "orphan data",
            rb_failures, trans_id.c_str());
      }
      http_code     = http_status_code::INTERNAL_SERVER_ERROR;
      response_body = make_problem_detail(
          http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
          "PFD transaction aborted: UDR write failed for app " + app_id);
      return;
    }
    pfd_rollback.mark_committed(app_id);
  }

  // All UDR writes succeeded — commit local state (deferred commit)
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    m_pfd_trans_sessions[trans_id] = std::move(app_map);
    m_pfd_trans2scs_id[trans_id]   = scs_as_id;
  }

  response_body            = body;
  response_body["transId"] = trans_id;
  http_code = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "PFD_TX", scs_as_id, trans_id,
      http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }
  std::map<std::string, oai::_3gpp::model::PfdDataForApp> trans_body;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code = http_status_code::FORBIDDEN;
      return;
    }
    trans_body = it->second;
    m_pfd_trans_sessions.erase(it);
    m_pfd_trans2scs_id.erase(trans_id);
  }

  // Delete each application's PFD data from UDR
  for (const auto& [app_id, _] : trans_body) {
    m_nef_client->udr_delete_pfd_data(app_id);
  }
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "PFD_TX", scs_as_id, trans_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  std::shared_lock lock(m_pfd_mutex);
  auto it = m_pfd_trans_sessions.find(trans_id);
  if (it == m_pfd_trans_sessions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD transaction not found");
    return;
  }
  auto owner_it = m_pfd_trans2scs_id.find(trans_id);
  if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this resource");
    return;
  }
  const auto& app_map = it->second;
  auto app_it         = app_map.find(app_id);
  if (app_it == app_map.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Application PFD not found in transaction");
    return;
  }
  to_json(response_body, app_it->second);
  response_body["appId"] = app_id;
  http_code              = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  bool is_create = false;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "PFD transaction not found");
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }
    // Typed parse + validate new app data
    oai::_3gpp::model::PfdDataForApp new_app;
    try {
      from_json(body, new_app);
      new_app.validate();
    } catch (const nlohmann::json::exception& e) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", e.what());
      return;
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
          e.what());
      return;
    }
    is_create          = (it->second.find(app_id) == it->second.end());
    it->second[app_id] = new_app;
  }

  nlohmann::json new_app_json;
  {
    std::shared_lock rlock(m_pfd_mutex);
    to_json(new_app_json, m_pfd_trans_sessions.at(trans_id).at(app_id));
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, new_app_json)) {
    Logger::nef_app().warn(
        "UDR PFD app PUT failed for app: %s", app_id.c_str());
  }

  response_body          = new_app_json;
  response_body["appId"] = app_id;
  http_code = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "PFD_APP", scs_as_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& patch_body,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  nlohmann::json patched;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "PFD transaction not found");
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }
    auto app_it = it->second.find(app_id);
    if (app_it == it->second.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "Application PFD not found in transaction");
      return;
    }
    // Serialize existing to JSON, apply patch, re-parse as typed
    to_json(patched, app_it->second);
    patched.merge_patch(patch_body);
    // Re-parse patched JSON back to typed and update store
    oai::_3gpp::model::PfdDataForApp patched_app;
    try {
      from_json(patched, patched_app);
      patched_app.validate();
    } catch (const std::exception& e) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
          std::string("Patched body invalid: ") + e.what());
      return;
    }
    app_it->second = patched_app;
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, patched)) {
    Logger::nef_app().warn(
        "UDR PFD app PATCH failed for app: %s", app_id.c_str());
  }

  response_body          = patched;
  response_body["appId"] = app_id;
  http_code              = http_status_code::OK;
  nef_audit::log("PATCH", "PFD_APP", scs_as_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code = http_status_code::FORBIDDEN;
      return;
    }
    if (it->second.find(app_id) == it->second.end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    it->second.erase(app_id);
  }
  m_nef_client->udr_delete_pfd_data(app_id);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "PFD_APP", scs_as_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_list_transactions(
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [transaction_id, transaction] : m_nnef_pfd_transactions) {
    response_body.push_back(transaction);
  }
  http_code = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_put_transaction(
    const std::string& transaction_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  nlohmann::json applications;
  std::string error_detail;
  if (!extract_nnef_pfd_transaction_apps(body, applications, error_detail)) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    return;
  }

  nlohmann::json transaction =
      make_nnef_pfd_transaction(transaction_id, body, applications);

  // Check create-vs-update before UDR writes (deferred local commit)
  bool is_create;
  nlohmann::json previous_transaction;
  {
    std::shared_lock lock(m_nnef_pfd_transactions_mutex);
    auto it   = m_nnef_pfd_transactions.find(transaction_id);
    is_create = (it == m_nnef_pfd_transactions.end());
    if (!is_create) {
      previous_transaction = it->second;
    }
  }

  // Write each app to UDR atomically — rollback committed apps on failure
  PfdRollbackTracker pfd_rollback;
  for (const auto& [app_id, app_body] : applications.items()) {
    if (!m_nef_client->udr_put_pfd_data(app_id, app_body)) {
      Logger::nef_app().error(
          "UDR PFD write failed for Nnef app '%s' in trans '%s'; "
          "rolling back %zu committed app(s)",
          app_id.c_str(), transaction_id.c_str(),
          pfd_rollback.committed_count());
      const int rb_failures = pfd_rollback.execute(
          [this](const std::string& rid) {
            return m_nef_client->udr_delete_pfd_data(rid);
          },
          [&transaction_id](const std::string& rid) {
            Logger::nef_app().error(
                "Rollback delete failed for Nnef app '%s' in trans '%s'",
                rid.c_str(), transaction_id.c_str());
          });
      if (rb_failures > 0) {
        Logger::nef_app().error(
            "%d rollback failure(s) in Nnef trans '%s' — UDR may retain orphan "
            "data",
            rb_failures, transaction_id.c_str());
      }
      http_code     = http_status_code::INTERNAL_SERVER_ERROR;
      response_body = make_problem_detail(
          http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
          "PFD transaction aborted: UDR write failed for app " + app_id);
      return;
    }
    pfd_rollback.mark_committed(app_id);
  }

  // All UDR writes succeeded — commit local state
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    m_nnef_pfd_transactions[transaction_id] = transaction;
  }

  if (!is_create && previous_transaction.contains("applications") &&
      previous_transaction["applications"].is_object()) {
    for (const auto& [app_id, _] :
         previous_transaction["applications"].items()) {
      if (applications.contains(app_id)) continue;
      if (!m_nef_client->udr_delete_pfd_data(app_id)) {
        Logger::nef_app().warn(
            "UDR PFD delete failed for removed Nnef_PFDmanagement app: %s in "
            "transaction: %s",
            app_id.c_str(), transaction_id.c_str());
      }
    }
  }

  response_body = transaction;
  http_code     = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "NNEF_PFD_TX", "", transaction_id,
      http_code);

  // Notify SBI PFD subscribers about the PFD change
  if (transaction.contains("applications") &&
      transaction["applications"].is_object()) {
    for (const auto& [app_id, app_data] : transaction["applications"].items()) {
      notify_nnef_pfd_subscribers("PFD_CHANGE", app_id, app_data);
    }
  }
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_get_transaction(
    const std::string& transaction_id, nlohmann::json& response_body,
    int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto it = m_nnef_pfd_transactions.find(transaction_id);
  if (it == m_nnef_pfd_transactions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD transaction not found");
    return;
  }
  response_body = it->second;
  http_code     = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_delete_transaction(
    const std::string& transaction_id, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }
  nlohmann::json transaction;
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto it = m_nnef_pfd_transactions.find(transaction_id);
    if (it == m_nnef_pfd_transactions.end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    transaction = it->second;
    m_nnef_pfd_transactions.erase(it);
  }

  if (transaction.contains("applications") &&
      transaction["applications"].is_object()) {
    for (const auto& [app_id, _] : transaction["applications"].items()) {
      if (!m_nef_client->udr_delete_pfd_data(app_id)) {
        Logger::nef_app().warn(
            "UDR PFD delete failed for Nnef_PFDmanagement app: %s in "
            "transaction: %s",
            app_id.c_str(), transaction_id.c_str());
      }
    }
  }
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "NNEF_PFD_TX", "", transaction_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_get_app(
    const std::string& transaction_id, const std::string& app_id,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto transaction_it = m_nnef_pfd_transactions.find(transaction_id);
  if (transaction_it == m_nnef_pfd_transactions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD transaction not found");
    return;
  }

  const auto& transaction = transaction_it->second;
  if (!transaction.contains("applications") ||
      !transaction["applications"].is_object() ||
      !transaction["applications"].contains(app_id)) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Application PFD not found in transaction");
    return;
  }

  response_body = transaction["applications"][app_id];
  http_code     = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_put_app(
    const std::string& transaction_id, const std::string& app_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::string error_detail;
  nlohmann::json normalized_app;
  if (!normalize_nnef_pfd_app_data(
          app_id, body, normalized_app, error_detail)) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    return;
  }

  bool is_create = false;
  nlohmann::json transaction_snapshot;
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto& transaction = m_nnef_pfd_transactions[transaction_id];
    if (!transaction.is_object()) {
      transaction = nlohmann::json::object();
    }
    transaction["transactionId"] = transaction_id;
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object()) {
      transaction["applications"] = nlohmann::json::object();
    }
    is_create = !transaction["applications"].contains(app_id);
    transaction["applications"][app_id] = normalized_app;
    if (transaction.contains("pfdDatas")) {
      transaction.erase("pfdDatas");
    }
    transaction_snapshot = transaction;
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, normalized_app)) {
    Logger::nef_app().warn(
        "UDR PFD app PUT failed for Nnef_PFDmanagement app: %s",
        app_id.c_str());
  }

  response_body = transaction_snapshot["applications"][app_id];
  http_code     = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "NNEF_PFD_APP", "", app_id, http_code);
  // Notify SBI PFD subscribers
  notify_nnef_pfd_subscribers("PFD_CHANGE", app_id, normalized_app);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_delete_app(
    const std::string& transaction_id, const std::string& app_id,
    int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto transaction_it = m_nnef_pfd_transactions.find(transaction_id);
    if (transaction_it == m_nnef_pfd_transactions.end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    auto& transaction = transaction_it->second;
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object() ||
        !transaction["applications"].contains(app_id)) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    transaction["applications"].erase(app_id);
  }

  if (!m_nef_client->udr_delete_pfd_data(app_id)) {
    Logger::nef_app().warn(
        "UDR PFD app DELETE failed for Nnef_PFDmanagement app: %s",
        app_id.c_str());
  }
  // Notify SBI PFD subscribers about removal
  notify_nnef_pfd_subscribers("PFD_REMOVE", app_id, nullptr);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "NNEF_PFD_APP", "", app_id, http_code);
}

// Nnef_PFDmanagement — GET /applications
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  // Returns all PFD apps across all transactions, filtered by app-ids param.
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [trans_id, transaction] : m_nnef_pfd_transactions) {
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object())
      continue;
    for (const auto& [app_id, app_data] : transaction["applications"].items()) {
      if (!app_ids_filter.empty()) {
        bool found = false;
        for (const auto& f : app_ids_filter) {
          if (f == app_id) {
            found = true;
            break;
          }
        }
        if (!found) continue;
      }
      nlohmann::json entry = app_data;
      entry["transId"]     = trans_id;
      response_body.push_back(entry);
    }
  }
  http_code = http_status_code::OK;
}

// Nnef_PFDmanagement — POST /applications/partial-pull
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_partial_pull(
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  // Partial-pull: client supplies optional list of appIds and optional
  // lastQueryTime. Return matching apps (queried from UDR; fallback to local
  // cache if UDR unavailable).
  std::vector<std::string> requested_ids;
  if (body.contains("appIds") && body["appIds"].is_array()) {
    for (const auto& v : body["appIds"]) {
      if (v.is_string()) requested_ids.push_back(v.get<std::string>());
    }
  }

  response_body = nlohmann::json::array();
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  for (const auto& [trans_id, transaction] : m_nnef_pfd_transactions) {
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object())
      continue;
    for (const auto& [app_id, app_data] : transaction["applications"].items()) {
      if (!requested_ids.empty()) {
        bool found = false;
        for (const auto& f : requested_ids)
          if (f == app_id) {
            found = true;
            break;
          }
        if (!found) continue;
      }
      // Attempt to refresh from UDR for accurate data
      nlohmann::json udr_result;
      uint32_t udr_code = 0;
      m_nef_client->udr_get_pfd_data(app_id, udr_result, udr_code);
      nlohmann::json entry =
          (udr_code == http_status_code::OK) ? udr_result : app_data;
      entry["applicationId"] = app_id;
      response_body.push_back(entry);
    }
  }
  http_code = http_status_code::OK;
}

// Nnef_PFDmanagement — subscription CRUD
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_create(
    const nlohmann::json& body, std::string& sub_id,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  // Normalize: wire uses "notifUri", model uses "notifyUri"
  nlohmann::json normalized = body;
  if (normalized.contains("notifUri")) {
    normalized["notifyUri"] = normalized["notifUri"];
    normalized.erase("notifUri");
  }

  oai::_3gpp::model::PfdSubscription sub;
  try {
    from_json(normalized, sub);
    sub.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }

  if (sub.getNotifyUri().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri is required");
    return;
  }
  const std::string uri_err = validate_callback_uri(sub.getNotifyUri());
  if (!uri_err.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
    return;
  }

  generate_af_subscription_id(sub_id);

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_subscriptions_mutex);
    m_nnef_pfd_subscriptions[sub_id] = sub;
  }

  // Build response: serialize typed, denormalize back to wire key
  to_json(response_body, sub);
  if (response_body.contains("notifyUri")) {
    response_body["notifUri"] = response_body["notifyUri"];
    response_body.erase("notifyUri");
  }
  response_body["subId"] = sub_id;
  http_code              = http_status_code::CREATED;
  nef_audit::log("CREATE", "NNEF_PFD_SUB", "", sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_get(
    const std::string& sub_id, nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::shared_lock lock(m_nnef_pfd_subscriptions_mutex);
  auto it = m_nnef_pfd_subscriptions.find(sub_id);
  if (it == m_nnef_pfd_subscriptions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD subscription not found");
    return;
  }
  // Serialize typed, denormalize wire key
  to_json(response_body, it->second);
  if (response_body.contains("notifyUri")) {
    response_body["notifUri"] = response_body["notifyUri"];
    response_body.erase("notifyUri");
  }
  response_body["subId"] = sub_id;
  http_code              = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_put(
    const std::string& sub_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  // Normalize: wire uses "notifUri", model uses "notifyUri"
  nlohmann::json normalized = body;
  if (normalized.contains("notifUri")) {
    normalized["notifyUri"] = normalized["notifUri"];
    normalized.erase("notifUri");
  }

  oai::_3gpp::model::PfdSubscription sub;
  try {
    from_json(normalized, sub);
    sub.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }

  if (sub.getNotifyUri().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri is required");
    return;
  }
  const std::string uri_err = validate_callback_uri(sub.getNotifyUri());
  if (!uri_err.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_subscriptions_mutex);
    auto it = m_nnef_pfd_subscriptions.find(sub_id);
    if (it == m_nnef_pfd_subscriptions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "PFD subscription not found");
      return;
    }
    it->second = sub;
  }

  // Build response: denormalize wire key
  to_json(response_body, sub);
  if (response_body.contains("notifyUri")) {
    response_body["notifUri"] = response_body["notifyUri"];
    response_body.erase("notifyUri");
  }
  response_body["subId"] = sub_id;
  http_code              = http_status_code::OK;
  nef_audit::log("UPDATE", "NNEF_PFD_SUB", "", sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_delete(
    const std::string& sub_id, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }
  const std::lock_guard<std::shared_mutex> lock(m_nnef_pfd_subscriptions_mutex);
  auto it = m_nnef_pfd_subscriptions.find(sub_id);
  if (it == m_nnef_pfd_subscriptions.end()) {
    http_code = http_status_code::NOT_FOUND;
    return;
  }
  m_nnef_pfd_subscriptions.erase(it);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "NNEF_PFD_SUB", "", sub_id, http_code);
}

// Notify Nnef_PFDmanagement subscribers
//------------------------------------------------------------------------------
void nef_app::notify_nnef_pfd_subscribers(
    const std::string& event_type, const std::string& app_id,
    const nlohmann::json& pfd_data) {
  std::vector<std::pair<std::string, std::string>>
      targets;  // (sub_id, notifUri)
  {
    std::shared_lock lock(m_nnef_pfd_subscriptions_mutex);
    for (const auto& [sid, sub] : m_nnef_pfd_subscriptions) {
      const std::string& notify_uri = sub.getNotifyUri();
      if (notify_uri.empty()) continue;
      // Use typed overload: checks applicationIds filter
      if (!nnef_pfd_subscription_matches(sub, app_id)) continue;
      targets.emplace_back(sid, notify_uri);
    }
  }

  for (const auto& [sid, notif_uri] : targets) {
    nlohmann::json notif;
    notif["eventType"] = event_type;
    notif["appId"]     = app_id;
    if (!pfd_data.is_null()) notif["pfdData"] = pfd_data;

    const std::string notif_str = notif.dump();
    m_notification_pool->enqueue([this, notif_uri, notif_str, sid]() {
      m_nef_client->forward_notification_to_af(notif_uri, notif_str);
    });
  }
}

// Analytics UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Analytics subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }
  if (!body.contains("analyEventsSubs") || !body.contains("notifUri") ||
      !body.contains("notifId")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "analyEventsSubs, notifUri, and notifId are required");
    return;
  }
  // Type and length validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_field(body, "notifUri", true, 2048);
    if (err.empty()) err = validate_string_field(body, "notifId", true, 256);
    if (err.empty() && !body["analyEventsSubs"].is_array())
      err = "analyEventsSubs: must be an array";
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }
  // SSRF protection: validate the callback URI before updating stored state
  {
    const std::string uri_err =
        validate_callback_uri(body["notifUri"].get<std::string>());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
      return;
    }
  }
  sub->set_subscription_data(body);
  if (body.contains("notifUri") && body["notifUri"].is_string()) {
    sub->set_notification_uri(body["notifUri"].get<std::string>());
  }
  response_body          = body;
  response_body["subId"] = sub_id;
  http_code              = http_status_code::OK;
  nef_audit::log("UPDATE", "ANA", scs_as_id, sub_id, http_code);
}

// ═══════════════════════════════════════════════════════════════════════════
// True-async handler split — P2 proving ground (plan §A / §D).
//
// CANONICAL PATTERN (every entry/cont_* below follows it):
//   <op>()   : set_request_bearer_token(token) → run the sync handler's
//              pre-southbound work verbatim (authorize → typed parse/validate →
//              SSRF → local-store) with each early return doing
//              clear_request_bearer_token() + sink(code, body) + return →
//              clear the token → capture by VALUE the state the continuation
//              needs → FIRE the async southbound call → RETURN. The dispatcher
//              worker is never parked on the SBI RTT.
//   cont_*   : runs on oai-http-io (or inline on a synchronous fast-path).
//              Holds NO token. Re-locks every store it touches. Applies the
//              handler's §D failure policy. Builds the SAME (status, body) the
//              sync handler produced and completes the deferred via `sink`.
// ═══════════════════════════════════════════════════════════════════════════

// ─── #1 monitoring_event_subscription_create — single, FATAL-502 ───────────
// Mirrors handle_monitoring_event_subscription_create (:1470-1594): the
// pre-southbound block (authorize/validate/SSRF/store) is byte-identical; the
// AMF subscribe (sync :1573) becomes an async fire; the rollback (:1574-1576)
// and post-wiring (:1584-1592) move into cont_monitoring_event_subscribe.
void nef_app::monitoring_event_subscribe(
    const std::string& scs_as_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info(
      "Create monitoring event subscription for SCS/AS: %s", scs_as_id.c_str());

  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }

  if (!body.contains("monitoringType") ||
      !body.contains("notificationDestination")) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            "monitoringType and notificationDestination are required")
            .dump());
  }

  {
    static const std::unordered_set<std::string> kMonitoringTypes = {
        "LOSS_OF_CONNECTIVITY",
        "UE_REACHABILITY",
        "LOCATION_REPORTING",
        "CHANGE_OF_IMSI_IMEI_ASSOCIATION",
        "ROAMING_STATUS",
        "COMMUNICATION_FAILURE",
        "AVAILABILITY_AFTER_DDN_FAILURE",
        "NUMBER_OF_UES_IN_AN_AREA",
        "PDN_CONNECTIVITY_STATUS",
        "DOWNLINK_DATA_DELIVERY_STATUS",
        "API_SUPPORT_CAPABILITY",
        "NUM_OF_REGD_UES",
        "NUM_OF_ESTD_PDU_SESSIONS",
        "AREA_OF_INTEREST"};
    std::string err;
    if (err.empty()) err = validate_string_param(scs_as_id, "scsAsId", 256);
    if (err.empty())
      err = validate_enum_field(body, "monitoringType", kMonitoringTypes, true);
    if (err.empty())
      err = validate_string_field(body, "notificationDestination", true, 2048);
    if (!err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              err)
              .dump());
    }
  }

  {
    const std::string uri_err = validate_callback_uri(
        body["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST, "Bad Request",
              "notificationDestination: " + uri_err)
              .dump());
    }
  }

  std::string sub_id;
  generate_af_subscription_id(sub_id);
  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(sub_id);
  sub->set_scs_as_id(scs_as_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_MONITORING_EVENT);
  sub->set_target_nf_type(nf_type_t::NF_TYPE_AMF);
  sub->set_subscription_data(body);
  if (body.contains("notificationDestination")) {
    sub->set_notification_uri(
        body["notificationDestination"].get<std::string>());
  }
  if (body.contains("monitorExpireTime") &&
      body["monitorExpireTime"].is_string()) {
    std::chrono::system_clock::time_point expire_time;
    if (!parse_monitor_expire_time(
            body["monitorExpireTime"].get<std::string>(), expire_time)) {
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST, "Bad Request",
              "Invalid monitorExpireTime format")
              .dump());
    }
    sub->set_expire_time(expire_time);
  }

  add_subscription(sub_id, sub);
  ensure_af_profile(scs_as_id, sub_id);
  clear_request_bearer_token();

  // FIRE: AMF event-exposure subscribe (single call, phase-1, dispatcher
  // worker → discover_nf inside the wrapper is safe per §A.1b).
  m_nef_client->subscribe_amf_event_exposure_async(
      body, [this, scs_as_id, sub_id, body,
             sink = std::move(sink)](oai::http::response r) mutable {
        cont_monitoring_event_subscribe(
            scs_as_id, sub_id, body, std::move(r), std::move(sink));
      });
}

void nef_app::cont_monitoring_event_subscribe(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_monitoring_event_subscribe on thread, sub_id=%s status=%d",
      sub_id.c_str(), r.status_code);
  const std::string amf_sub_id = sbi_ok(r) ? nef_async_parse_amf_sub_id(r) : "";
  // FATAL-502: AMF failure (or 2xx without a usable id) rolls back local state
  // and fails the request (mirrors :1574-1581).
  if (!sbi_ok(r) || amf_sub_id.empty()) {
    Logger::nef_app().warn("Failed to subscribe to AMF event exposure (async)");
    remove_subscription(sub_id);
    release_af_profile_subscription(scs_as_id, sub_id);
    const int code = sbi_error_http_code(r);
    return sink(
        code,
        make_problem_detail(
            code, "Bad Gateway", "Failed to create AMF monitoring subscription")
            .dump());
  }

  if (auto sub = find_subscription(sub_id)) {
    sub->set_nf_subscription_id(amf_sub_id);
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[amf_sub_id] = sub_id;
  }

  nlohmann::json response_body = body;
  response_body["subId"]       = sub_id;
  nef_audit::log("CREATE", "ME", scs_as_id, sub_id, http_status_code::CREATED);
  sink(http_status_code::CREATED, response_body.dump());
}

// ─── #20 pfd_app_put — single, BEST-EFFORT ─────────────────────────────────
// Mirrors handle_pfd_app_put (:3562-3630): the not-found/forbidden/parse block
// + local store are byte-identical; the UDR PUT (sync :3620) becomes an async
// fire; the response is built from the locally-stored app json regardless of
// the UDR result (BEST-EFFORT — UDR failure is WARN-only, mirrors :3621-3623).
void nef_app::pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }

  bool is_create = false;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "Not Found",
              "PFD transaction not found")
              .dump());
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN, "Forbidden",
              "AF is not allowed to access this resource")
              .dump());
    }
    oai::_3gpp::model::PfdDataForApp new_app;
    try {
      from_json(body, new_app);
      new_app.validate();
    } catch (const nlohmann::json::exception& e) {
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST, "Bad Request", e.what())
              .dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              e.what())
              .dump());
    }
    is_create          = (it->second.find(app_id) == it->second.end());
    it->second[app_id] = new_app;
  }

  nlohmann::json new_app_json;
  {
    std::shared_lock rlock(m_pfd_mutex);
    to_json(new_app_json, m_pfd_trans_sessions.at(trans_id).at(app_id));
  }
  clear_request_bearer_token();

  // FIRE: UDR PFD PUT (single call, phase-1).
  m_nef_client->udr_put_pfd_data_async(
      app_id, new_app_json,
      [this, scs_as_id, app_id, new_app_json, is_create,
       sink = std::move(sink)](oai::http::response r) mutable {
        cont_pfd_app_put(
            scs_as_id, app_id, std::move(new_app_json), is_create, std::move(r),
            std::move(sink));
      });
}

void nef_app::cont_pfd_app_put(
    const std::string& scs_as_id, const std::string& app_id,
    nlohmann::json new_app_json, bool is_create, oai::http::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_app_put app_id=%s status=%d", app_id.c_str(), r.status_code);
  // BEST-EFFORT: UDR failure is WARN-only; the response is the locally-stored
  // app data regardless of the southbound outcome (mirrors :3621-3627).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app PUT failed for app: %s", app_id.c_str());
  }
  nlohmann::json response_body = std::move(new_app_json);
  response_body["appId"]       = app_id;
  const int http_code =
      is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "PFD_APP", scs_as_id, app_id, http_code);
  sink(http_code, response_body.dump());
}

// ─── #4 traffic_influence_update — single, FATAL-502 ───────────────────────
// Mirrors handle_traffic_influence_update (:1853-1987): pre-southbound block
// (authorize/parse/validate/SSRF + owner lookup + pcf_policy_id resolve)
// byte-identical; PCF PATCH (sync :1960) becomes an async fire; the failure
// branch (:1962-1969) and the post-commit session overwrite + 200
// (:1972-1985) move into cont_ti_update. §A.2b re-check applied on commit.
void nef_app::ti_update(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
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
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
            std::string("Validation failed: ") + e.what())
            .dump());
  }

  if (!ti.afAppIdIsSet() && !ti.trafficFiltersIsSet() &&
      !ti.ethTrafficFiltersIsSet()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
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
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              err)
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
              http_status_code::BAD_REQUEST, "Bad Request",
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
              http_status_code::NOT_FOUND, "Not Found", "TI session not found")
              .dump());
    }
    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN, "Forbidden",
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
            http_status_code::BAD_GATEWAY, "Bad Gateway",
            "Missing PCF policy identifier for TI session")
            .dump());
  }
  clear_request_bearer_token();

  // FIRE: PCF policy-auth update (single call, phase-1).
  m_nef_client->update_pcf_policy_auth_async(
      pcf_policy_id, body,
      [this, af_id, ti_id, body,
       sink = std::move(sink)](oai::http::response r) mutable {
        cont_ti_update(af_id, ti_id, body, std::move(r), std::move(sink));
      });
}

void nef_app::cont_ti_update(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_update ti_id=%s status=%d", ti_id.c_str(), r.status_code);
  // FATAL-502 (mirrors :1962-1969).
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

  // Commit the new session body; §A.2b re-check (a concurrent delete may have
  // removed ti_id while PCF was in flight → benign 404, mirrors :1974-1980).
  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "Not Found", "TI session not found")
              .dump());
    }
    session_it->second = body;
  }
  nef_audit::log("UPDATE", "TI", af_id, ti_id, http_status_code::OK);
  sink(http_status_code::OK, body.dump());
}

// ─── #5 traffic_influence_patch — single, FATAL-502 ────────────────────────
// Mirrors handle_traffic_influence_patch (:3105-3199): pre-southbound block
// (authorize/typed-patch-validate + owner lookup + merge_patch into a local
// patched_copy + pcf_policy_id resolve) byte-identical; PCF PATCH (sync :3172,
// on patched_copy) becomes an async fire; failure (:3174-3181) and post-commit
// session overwrite + 200 (:3184-3198) move into cont_ti_patch. §A.2b re-check.
void nef_app::ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& patch_body, const std::string& token,
    response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
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
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
            std::string("Validation failed: ") + e.what())
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
              http_status_code::NOT_FOUND, "Not Found", "TI session not found")
              .dump());
    }
    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN, "Forbidden",
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
            http_status_code::BAD_GATEWAY, "Bad Gateway",
            "Missing PCF policy identifier for TI session")
            .dump());
  }
  clear_request_bearer_token();

  // FIRE: PCF policy-auth update with the merged copy (single call, phase-1).
  m_nef_client->update_pcf_policy_auth_async(
      pcf_policy_id, patched_copy,
      [this, af_id, ti_id, patched_copy,
       sink = std::move(sink)](oai::http::response r) mutable {
        cont_ti_patch(
            af_id, ti_id, ti_id, std::move(patched_copy), std::move(r),
            std::move(sink));
      });
}

void nef_app::cont_ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const std::string& app_session_id, nlohmann::json patched_copy,
    oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_patch ti_id=%s status=%d", app_session_id.c_str(),
      r.status_code);
  // FATAL-502 (mirrors :3174-3181).
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

  // Commit; §A.2b re-check (mirrors :3186-3192).
  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(app_session_id);
    if (session_it == m_ti_sessions.end()) {
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "Not Found", "TI session not found")
              .dump());
    }
    session_it->second = patched_copy;
  }
  nlohmann::json response_body = std::move(patched_copy);
  response_body["afTransId"]   = app_session_id;
  nef_audit::log("PATCH", "TI", af_id, app_session_id, http_status_code::OK);
  sink(http_status_code::OK, response_body.dump());
}

// ─── #7 qos_subscription_create — single, FATAL-500 ────────────────────────
// Mirrors handle_qos_subscription_create (:2602-2753): the pre-southbound block
// (authorize/typed-parse/validate/SSRF + store + ensure_af_profile +
// build_pcf_qos_body) is byte-identical; the PCF create (sync :2709) becomes an
// async fire; the FATAL-500 failure branch (:2713-2721, INTERNAL_SERVER_ERROR
// per TS 29.522 §4.4.9 — NOT 502), the wiring (:2723-2736) and the success body
// with the relative `self` URI (:2742-2752) move into cont_qos_create. The
// Location/self → absolute URI rewrite stays in the adapter header sink.
//
// NOTE on parity: the async path resolves the PCF appSessionId from the raw
// response (JSON appSessionId, else Location header — same precedence as the
// sync create_pcf_policy_auth, nef_client.cpp:933-945) and then applies the
// SAME is_valid_app_session_id() guard the sync handler applies (:2713). For a
// southbound timeout/0/4xx/5xx the parsed id is empty/invalid → 500, matching
// the sync `!pcf_ok` branch.
void nef_app::qos_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info("QoS subscription create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }

  oai::_3gpp::model::AsSessionWithQoSSubscription req_data = {};
  try {
    from_json(body, req_data);
    req_data.validate();
  } catch (const nlohmann::json::exception& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
            std::string("Validation failed: ") + e.what())
            .dump());
  }

  if (req_data.getNotificationDestination().empty()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            "notificationDestination is required")
            .dump());
  }

  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty())
      err = validate_string_field(body, "notificationDestination", true, 2048);
    if (!err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              err)
              .dump());
    }
  }

  {
    const std::string uri_err =
        validate_callback_uri(req_data.getNotificationDestination());
    if (!uri_err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST, "Bad Request",
              "notificationDestination: " + uri_err)
              .dump());
    }
  }

  std::string qos_sub_id;
  generate_af_subscription_id(qos_sub_id);
  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(qos_sub_id);
  sub->set_scs_as_id(af_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING);
  sub->set_target_nf_type(nf_type_t::NF_TYPE_PCF);
  sub->set_subscription_data(body);

  add_subscription(qos_sub_id, sub);
  ensure_af_profile(af_id, qos_sub_id);

  const std::string evsubsc_notif_uri =
      nef_config_inst->get_local()->get_url() +
      oai::nef::api::nef_sbi_helper::NefNotifyBase +
      nef_config_inst->nef()->get_sbi().get_api_version() + "/notify/" +
      qos_sub_id;

  nlohmann::json pcf_body;
  std::string pcf_translate_err;
  if (!build_pcf_qos_body(
          req_data, evsubsc_notif_uri, pcf_body, pcf_translate_err)) {
    remove_subscription(qos_sub_id);
    release_af_profile_subscription(af_id, qos_sub_id);
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request", pcf_translate_err)
            .dump());
  }

  // Capture the typed request as JSON so the continuation can rebuild the
  // success body (set self URI) without re-parsing the AF body.
  nlohmann::json req_data_json;
  to_json(req_data_json, req_data);
  clear_request_bearer_token();

  // FIRE: PCF policy-auth create (single call, phase-1).
  m_nef_client->create_pcf_policy_auth_async(
      pcf_body, [this, af_id, qos_sub_id, req_data_json,
                 sink = std::move(sink)](oai::http::response r) mutable {
        cont_qos_create(
            af_id, qos_sub_id, std::move(req_data_json), std::move(r),
            std::move(sink));
      });
}

void nef_app::cont_qos_create(
    const std::string& af_id, const std::string& qos_sub_id,
    nlohmann::json req_data_json, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_qos_create qos_sub_id=%s status=%d", qos_sub_id.c_str(),
      r.status_code);
  const std::string pcf_app_session_id =
      sbi_ok(r) ? nef_async_parse_pcf_app_session_id(r) : "";
  // FATAL-500 (NOT 502): PCF must succeed AND return a valid appSessionId
  // (mirrors :2713-2721, INTERNAL_SERVER_ERROR per TS 29.522 §4.4.9).
  if (!sbi_ok(r) || !is_valid_app_session_id(pcf_app_session_id)) {
    remove_subscription(qos_sub_id);
    release_af_profile_subscription(af_id, qos_sub_id);
    return sink(
        http_status_code::INTERNAL_SERVER_ERROR,
        make_problem_detail(
            http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
            "Failed to create policy authorization in PCF")
            .dump());
  }

  // §A.2b re-check: a concurrent delete may have removed qos_sub_id while PCF
  // was in flight. If gone, do NOT resurrect — benign no-op (the AF delete
  // already won); send 204. (The freshly-created PCF app-session is left for
  // PCF/AF cleanup; QoS create has no compensating southbound delete in sync.)
  if (!find_subscription(qos_sub_id)) {
    Logger::nef_app().info(
        "QoS sub %s vanished during PCF create (concurrent delete); no-op",
        qos_sub_id.c_str());
    return sink(http_status_code::NO_CONTENT, "");
  }

  if (auto sub = find_subscription(qos_sub_id)) {
    sub->set_nf_subscription_id(pcf_app_session_id);
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_qos_mutex);
    m_qos_sub_id2pcf_app_session_id[qos_sub_id] = pcf_app_session_id;
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[qos_sub_id]         = qos_sub_id;
    m_nf2af_sub_id[pcf_app_session_id] = qos_sub_id;
  }
  if (auto sub = find_subscription(qos_sub_id)) {
    if (req_data_json.contains("notificationDestination") &&
        req_data_json["notificationDestination"].is_string()) {
      sub->set_notification_uri(
          req_data_json["notificationDestination"].get<std::string>());
    }
  }

  const std::string self_uri =
      oai::nef::api::nef_sbi_helper::NefQosMonitoringBase +
      nef_config_inst->nef()->get_sbi().get_api_version() + "/" + af_id + "/" +
      oai::nef::api::nef_sbi_helper::NefResourceSubscriptions + "/" +
      qos_sub_id;
  req_data_json["self"] = self_uri;
  if (auto sub = find_subscription(qos_sub_id)) sub->set_self(self_uri);
  nef_audit::log("CREATE", "QOS", af_id, qos_sub_id, http_status_code::CREATED);
  // The adapter header sink rewrites the relative `self` to an absolute URI and
  // emits the Location header + application/json content-type on 201.
  sink(http_status_code::CREATED, req_data_json.dump());
}

// ─── #3 traffic_influence_create — CHAINED PCF→UDR ─────────────────────────
// FATAL-502 on the PCF leg; BEST-EFFORT on the UDR leg (mirrors
// handle_traffic_influence_create, :1684-1849). §A.1c fix (a): phase-1
// pre-resolves BOTH the PCF and UDR endpoints via discover_nf on the dispatcher
// worker, threads them by value, and the continuations fire the discovery-free
// *_at_async variants — so discover_nf NEVER runs on oai-http-io (no io-pool
// self-deadlock).
//
//   phase-1            : authorize/typed-parse/validate/SSRF + local store +
//                        add_subscription + resolve PCF & UDR endpoints →
//                        FIRE create_pcf_policy_auth_at_async(pcf_ep).
//   cont_ti_create_pcf : on PCF fail → LOCAL-ONLY rollback + 502 (PCF never
//                        committed, :1788-1821). On PCF success → §A.2b
//                        re-check, wire m_ti_id2pcf_policy_id + m_nf2af_sub_id,
//                        re-set ti_sub->set_nf_subscription_id (:1830), then
//                        FIRE udr_put_influence_data_at_async(udr_ep).
//   cont_ti_create_udr : BEST-EFFORT (UDR failure = WARN only, :1841-1843) →
//                        always 201 with the echoed body + afTransId (:1846).
void nef_app::ti_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info("Create TI subscription for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
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
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
            std::string("Validation failed: ") + e.what())
            .dump());
  }

  if (!ti.afAppIdIsSet() && !ti.trafficFiltersIsSet() &&
      !ti.ethTrafficFiltersIsSet()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
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
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              err)
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
              http_status_code::BAD_REQUEST, "Bad Request",
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

  // §A.1c fix (a): pre-resolve BOTH NF endpoints HERE (dispatcher worker).
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
            http_status_code::BAD_GATEWAY, "Bad Gateway",
            "Failed to create policy authorization in PCF")
            .dump());
  }
  clear_request_bearer_token();

  // FIRE leg #1: PCF create via the discovery-free *_at_async variant. pcf_ep
  // is threaded into the continuation so the §A.2b vanish-path compensating
  // delete can ALSO use a discovery-free *_at_async variant — no discover_nf
  // ever runs on the oai-http-io thread (§A.1c / risk-5).
  m_nef_client->create_pcf_policy_auth_at_async(
      pcf_ep, body,
      [this, af_id, body, ti_id, pcf_ep, udr_ep, ti_sub,
       sink = std::move(sink)](oai::http::response r) mutable {
        cont_ti_create_pcf(
            af_id, body, ti_id, pcf_ep, udr_ep, ti_sub, std::move(r),
            std::move(sink));
      });
}

void nef_app::cont_ti_create_pcf(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& ti_id, const std::string& pcf_ep,
    const std::string& udr_ep, std::shared_ptr<nef_subscription> ti_sub,
    oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_create_pcf ti_id=%s status=%d", ti_id.c_str(), r.status_code);
  const std::string pcf_policy_id =
      sbi_ok(r) ? nef_async_parse_pcf_app_session_id(r) : "";
  // FATAL-502, LOCAL-ONLY rollback (PCF never committed; mirrors :1783-1821).
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

  // §A.2b RE-CHECK: a concurrent delete may have removed ti_id while PCF was in
  // flight. If gone, do NOT resurrect — best-effort async-delete the PCF
  // app-session we just created and complete with 204 (the AF delete already
  // won). The presence check + the surviving wiring happen under m_ti_mutex;
  // the compensating southbound delete is FIRED OUTSIDE the lock (no SBI fire
  // while holding a store mutex) and uses the discovery-free *_at_async variant
  // on the phase-1-resolved pcf_ep (no discover_nf on the io thread; §A.1c /
  // risk-5).
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
        pcf_ep, pcf_policy_id, [](oai::http::response) {});
    return sink(http_status_code::NO_CONTENT, "");
  }

  // Wire PCF policy ID → NEF sub ID for the notification return path. ti_sub is
  // the same shared_ptr add_subscription stored, so this re-set is observed by
  // the notification path (mirrors :1830).
  ti_sub->set_nf_subscription_id(pcf_policy_id);
  {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[pcf_policy_id] = ti_id;
  }

  // FIRE leg #2: UDR put-influence via the discovery-free *_at_async variant
  // on the pre-resolved udr_ep.
  m_nef_client->udr_put_influence_data_at_async(
      udr_ep, ti_id, body,
      [this, body, ti_id,
       sink = std::move(sink)](oai::http::response ur) mutable {
        cont_ti_create_udr(body, ti_id, std::move(ur), std::move(sink));
      });
}

void nef_app::cont_ti_create_udr(
    const nlohmann::json& body, const std::string& ti_id, oai::http::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_ti_create_udr ti_id=%s status=%d", ti_id.c_str(), r.status_code);
  // BEST-EFFORT leg: UDR failure is WARN-only and does NOT change the 201
  // (mirrors :1840-1843).
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

// ═══════════════════════════════════════════════════════════════════════════
// P3 — single-call Units 1-4 (15 handlers). Each entry/cont_* mirrors its
// sync handle_* (left intact) following the canonical pattern documented above.
// ═══════════════════════════════════════════════════════════════════════════

// ─── #2 monitoring_event_subscription_delete — single, BEST-EFFORT (204) ───
// Mirrors handle_monitoring_event_subscription_delete (:1599-1632): the
// authorize/owner block is byte-identical; the AMF unsubscribe (sync :1623,
// result unchecked) becomes an async fire; local cleanup + 204 (:1624-1631)
// run regardless of the southbound outcome (BEST-EFFORT, :1627 sets 204
// unconditionally).
void nef_app::monitoring_event_unsubscribe(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info(
      "Delete monitoring event subscription: %s", sub_id.c_str());

  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    clear_request_bearer_token();
    return sink(http_status_code::NOT_FOUND, "");
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }

  const std::string nf_sub_id = sub->get_nf_subscription_id();
  clear_request_bearer_token();

  // FIRE: AMF unsubscribe (single call, phase-1). If there is no NF sub id
  // there is nothing to unsubscribe — finish the cleanup inline (the sync path
  // skips the southbound call, :1622).
  if (nf_sub_id.empty()) {
    return cont_monitoring_event_unsubscribe(
        scs_as_id, sub_id, nf_sub_id, oai::http::response{}, std::move(sink));
  }
  m_nef_client->unsubscribe_amf_event_exposure_async(
      nf_sub_id, [this, scs_as_id, sub_id, nf_sub_id,
                  sink = std::move(sink)](oai::http::response r) mutable {
        cont_monitoring_event_unsubscribe(
            scs_as_id, sub_id, nf_sub_id, std::move(r), std::move(sink));
      });
}

void nef_app::cont_monitoring_event_unsubscribe(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& nf_sub_id, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_monitoring_event_unsubscribe sub_id=%s status=%d", sub_id.c_str(),
      r.status_code);
  // BEST-EFFORT: AMF result ignored (mirrors :1623-1627 — result unchecked).
  if (!nf_sub_id.empty() && !sbi_ok(r)) {
    Logger::nef_app().warn(
        "AMF event unsubscribe failed for nf_sub_id=%s (http=%d); local "
        "cleanup proceeds",
        nf_sub_id.c_str(), r.status_code);
  }
  if (!nf_sub_id.empty()) {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(nf_sub_id);
  }
  remove_subscription(sub_id);
  release_af_profile_subscription(scs_as_id, sub_id);
  nef_audit::log(
      "DELETE", "ME", scs_as_id, sub_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}

// ─── #8 qos_subscription_update (PUT) — single, BEST-EFFORT (200) ──────────
// Mirrors handle_qos_subscription_update (:240-374): authorize/typed-parse/
// validate/owner/immutability/app-session block + local store update are
// byte-identical; the PCF policy-auth update (sync :358) becomes an async fire;
// the response (:371-373) is the stored subscription data regardless of the PCF
// outcome (BEST-EFFORT, :356 PCF failure WARN-only). NOTE: when the PCF body
// cannot be translated the sync path fires NO southbound call (:364-369) — the
// async path mirrors this by finishing inline.
void nef_app::qos_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }

  oai::_3gpp::model::AsSessionWithQoSSubscription update_data;
  try {
    from_json(body, update_data);
    update_data.validate();
  } catch (const nlohmann::json::exception& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
            std::string("Validation failed: ") + e.what())
            .dump());
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    clear_request_bearer_token();
    return sink(
        http_status_code::NOT_FOUND,
        make_problem_detail(
            http_status_code::NOT_FOUND, "Not Found",
            "QoS subscription not found")
            .dump());
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF is not allowed to access this subscription")
            .dump());
  }
  {
    const std::string err =
        validate_string_field(body, "notificationDestination", false, 2048);
    if (!err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              err)
              .dump());
    }
  }
  if (!update_data.getNotificationDestination().empty()) {
    const std::string uri_err =
        validate_callback_uri(update_data.getNotificationDestination());
    if (!uri_err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST, "Bad Request",
              "notificationDestination: " + uri_err)
              .dump());
    }
  }
  {
    const nlohmann::json& stored = sub->get_subscription_data();
    for (const char* f :
         {"ueIpv4Addr", "ueIpv6Addr", "macAddr", "ipDomain", "dnn", "snssai",
          "supportedFeatures"}) {
      const bool in_req    = body.contains(f);
      const bool in_stored = stored.contains(f);
      if ((in_req && in_stored && body[f] != stored[f]) ||
          (in_req && !in_stored)) {
        clear_request_bearer_token();
        return sink(
            http_status_code::BAD_REQUEST,
            make_problem_detail(
                http_status_code::BAD_REQUEST, "Bad Request",
                std::string("Field '") + f + "' is immutable")
                .dump());
      }
    }
  }

  std::string app_session_id;
  {
    std::shared_lock<std::shared_mutex> l(m_qos_mutex);
    auto it = m_qos_sub_id2pcf_app_session_id.find(sub_id);
    if (it != m_qos_sub_id2pcf_app_session_id.end())
      app_session_id = it->second;
  }
  if (app_session_id.empty() || !is_valid_app_session_id(app_session_id)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::NOT_FOUND,
        make_problem_detail(
            http_status_code::NOT_FOUND, "Not Found",
            "No active PCF application session for this subscription")
            .dump());
  }

  sub->set_subscription_data(body);
  if (!update_data.getNotificationDestination().empty()) {
    sub->set_notification_uri(update_data.getNotificationDestination());
  }
  nlohmann::json response_data = sub->get_subscription_data();
  clear_request_bearer_token();

  // Build the PCF merge body; if it cannot be translated the sync path fires no
  // southbound call (:364-369) and still returns 200 — finish inline.
  nlohmann::json pcf_patch;
  std::string pcf_err;
  if (build_pcf_qos_body(
          update_data, /*evsubsc_notif_uri=*/"", pcf_patch, pcf_err)) {
    nlohmann::json merge =
        pcf_patch.value("ascReqData", nlohmann::json::object());
    merge.erase("evSubsc");  // subscription persists per §4.15.6.6a
    // FIRE: PCF policy-auth update (BEST-EFFORT).
    m_nef_client->update_pcf_policy_auth_async(
        app_session_id, merge,
        [this, scs_as_id, sub_id, response_data,
         sink = std::move(sink)](oai::http::response r) mutable {
          cont_qos_update(
              scs_as_id, sub_id, std::move(response_data), std::move(r),
              std::move(sink));
        });
  } else {
    Logger::nef_app().warn(
        "T8 PUT: cannot translate PCF body for sub=%s (%s); PCF subscription "
        "left unchanged",
        sub_id.c_str(), pcf_err.c_str());
    cont_qos_update(
        scs_as_id, sub_id, std::move(response_data), oai::http::response{},
        std::move(sink));
  }
}

void nef_app::cont_qos_update(
    const std::string& scs_as_id, const std::string& sub_id,
    nlohmann::json response_data, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_qos_update sub_id=%s status=%d", sub_id.c_str(), r.status_code);
  // BEST-EFFORT: PCF result is WARN-only; in-memory state already updated in
  // phase-1 (mirrors :358-363, success regardless, :369).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "T8 PUT: PCF update failed for sub=%s (http=%d); in-memory state "
        "updated, PCF best-effort",
        sub_id.c_str(), r.status_code);
  }
  nef_audit::log("UPDATE", "QOS", scs_as_id, sub_id, http_status_code::OK);
  sink(http_status_code::OK, response_data.dump());
}

// ─── #9 qos_subscription_patch — single, BEST-EFFORT (200) ─────────────────
// Mirrors handle_qos_subscription_patch (:3206-3348): authorize/owner/merge-
// patch/validate/SSRF/immutability/app-session block + local store update are
// byte-identical; the PCF update (sync :3332) becomes an async fire; the
// response (:3345-3347) is the patched body regardless of the PCF outcome
// (BEST-EFFORT, :3330 PCF failure WARN-only). When the PCF body cannot be
// translated the sync path fires no call (:3338-3343) — finish inline.
void nef_app::qos_patch(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& patch_body, const std::string& token,
    response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    clear_request_bearer_token();
    return sink(
        http_status_code::NOT_FOUND,
        make_problem_detail(
            http_status_code::NOT_FOUND, "Not Found",
            "QoS subscription not found")
            .dump());
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF is not allowed to access this subscription")
            .dump());
  }

  nlohmann::json patched = sub->get_subscription_data();
  patched.merge_patch(patch_body);

  oai::_3gpp::model::AsSessionWithQoSSubscription merged_data;
  try {
    from_json(patched, merged_data);
    merged_data.validate();
  } catch (const nlohmann::json::exception& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Invalid patched body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Patch produces an invalid subscription: ") + e.what())
            .dump());
  }
  if (merged_data.getNotificationDestination().empty()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            "notificationDestination is required and cannot be removed")
            .dump());
  }
  {
    const std::string uri_err =
        validate_callback_uri(merged_data.getNotificationDestination());
    if (!uri_err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST, "Bad Request",
              "notificationDestination: " + uri_err)
              .dump());
    }
  }
  {
    const nlohmann::json& stored = sub->get_subscription_data();
    for (const char* f :
         {"ueIpv4Addr", "ueIpv6Addr", "macAddr", "ipDomain", "dnn", "snssai",
          "supportedFeatures"}) {
      const bool in_patch  = patched.contains(f);
      const bool in_stored = stored.contains(f);
      if ((in_patch && in_stored && patched[f] != stored[f]) ||
          (in_patch && !in_stored)) {
        clear_request_bearer_token();
        return sink(
            http_status_code::BAD_REQUEST,
            make_problem_detail(
                http_status_code::BAD_REQUEST, "Bad Request",
                std::string("Field '") + f + "' is immutable")
                .dump());
      }
    }
  }

  std::string app_session_id;
  {
    std::shared_lock<std::shared_mutex> l(m_qos_mutex);
    auto it = m_qos_sub_id2pcf_app_session_id.find(sub_id);
    if (it != m_qos_sub_id2pcf_app_session_id.end())
      app_session_id = it->second;
  }
  if (app_session_id.empty() || !is_valid_app_session_id(app_session_id)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::NOT_FOUND,
        make_problem_detail(
            http_status_code::NOT_FOUND, "Not Found",
            "No active PCF application session for this subscription")
            .dump());
  }

  sub->set_subscription_data(patched);
  sub->set_notification_uri(merged_data.getNotificationDestination());
  clear_request_bearer_token();

  nlohmann::json pcf_patch;
  std::string pcf_err;
  if (build_pcf_qos_body(
          merged_data, /*evsubsc_notif_uri=*/"", pcf_patch, pcf_err)) {
    nlohmann::json merge =
        pcf_patch.value("ascReqData", nlohmann::json::object());
    merge.erase("evSubsc");
    // FIRE: PCF policy-auth update (BEST-EFFORT).
    m_nef_client->update_pcf_policy_auth_async(
        app_session_id, merge,
        [this, scs_as_id, sub_id, patched,
         sink = std::move(sink)](oai::http::response r) mutable {
          cont_qos_patch(
              scs_as_id, sub_id, std::move(patched), std::move(r),
              std::move(sink));
        });
  } else {
    Logger::nef_app().warn(
        "T8 PATCH: cannot translate PCF body for sub=%s (%s); PCF subscription "
        "left unchanged",
        sub_id.c_str(), pcf_err.c_str());
    cont_qos_patch(
        scs_as_id, sub_id, std::move(patched), oai::http::response{},
        std::move(sink));
  }
}

void nef_app::cont_qos_patch(
    const std::string& scs_as_id, const std::string& sub_id,
    nlohmann::json patched, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_qos_patch sub_id=%s status=%d", sub_id.c_str(), r.status_code);
  // BEST-EFFORT: PCF result WARN-only (mirrors :3332-3337, success regardless).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "T8 PATCH: PCF update failed for sub=%s (http=%d); in-memory state "
        "updated",
        sub_id.c_str(), r.status_code);
  }
  nef_audit::log("PATCH", "QOS", scs_as_id, sub_id, http_status_code::OK);
  sink(http_status_code::OK, patched.dump());
}

// ─── #10 qos_subscription_delete — single, BEST-EFFORT (204) ───────────────
// Mirrors handle_qos_subscription_delete (:2759-2788): authorize/owner block
// byte-identical; the SMF unsubscribe (sync :2779, result unchecked) becomes an
// async fire; local cleanup + 204 (:2780-2787) run regardless (BEST-EFFORT).
void nef_app::qos_delete(
    const std::string& af_id, const std::string& qos_sub_id,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  auto sub = find_subscription(qos_sub_id);
  if (!sub) {
    clear_request_bearer_token();
    return sink(http_status_code::NOT_FOUND, "");
  }
  if (!is_subscription_owner(sub, af_id)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }

  const std::string nf_sub_id = sub->get_nf_subscription_id();
  clear_request_bearer_token();

  if (nf_sub_id.empty()) {
    return cont_qos_delete(
        af_id, qos_sub_id, nf_sub_id, oai::http::response{}, std::move(sink));
  }
  // FIRE: SMF unsubscribe (single call, phase-1, BEST-EFFORT).
  m_nef_client->unsubscribe_smf_event_exposure_async(
      nf_sub_id, [this, af_id, qos_sub_id, nf_sub_id,
                  sink = std::move(sink)](oai::http::response r) mutable {
        cont_qos_delete(
            af_id, qos_sub_id, nf_sub_id, std::move(r), std::move(sink));
      });
}

void nef_app::cont_qos_delete(
    const std::string& af_id, const std::string& qos_sub_id,
    const std::string& nf_sub_id, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_qos_delete qos_sub_id=%s status=%d", qos_sub_id.c_str(),
      r.status_code);
  // BEST-EFFORT: SMF result ignored (mirrors :2779, result unchecked).
  if (!nf_sub_id.empty() && !sbi_ok(r)) {
    Logger::nef_app().warn(
        "SMF event unsubscribe failed for nf_sub_id=%s (http=%d); local "
        "cleanup proceeds",
        nf_sub_id.c_str(), r.status_code);
  }
  if (!nf_sub_id.empty()) {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(qos_sub_id);  // keyed by notifId == qos_sub_id (T5/T9)
  }
  remove_subscription(qos_sub_id);
  release_af_profile_subscription(af_id, qos_sub_id);
  nef_audit::log(
      "DELETE", "QOS", af_id, qos_sub_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}

// ─── #11 bdt_policy_create — single, SUCCESS-ON-3xx (201 incl. 303) ────────
// Mirrors handle_bdt_policy_create (:2144-2239): authorize/typed-parse/
// validate/bdtPolData/path-param block + bdt_id generation + local store
// (:2196-2202) byte-identical; the PCF BDT create (sync :2207) becomes an async
// fire. SUCCESS-ON-3xx: a 303 is success (the sync `!= SEE_OTHER` guard,
// :2208-2210); genuine failure → local rollback (:2215-2220) + 502 (:2221).
// On success the 201 body (:2233-2238) is built in the continuation. NOTE: the
// sync handler does NOT parse the PCF id into the AF response — it only stores
// pcf_bdt_id in m_bdt_id2pcf_policy_id (:2230). The async cont extracts that id
// via nef_async_parse_pcf_bdt_policy_id, which mirrors the sync
// create_pcf_bdt_policy precedence EXACTLY (nef_client.cpp:1166-1179): Location
// header last-path segment FIRST, then body bdtPolicyId → bdtRefId →
// bdtPolData.bdtRefId. (Must be Location-first — NOT the appSessionId-first
// app-session helper — or a body-only PCF response would leave the map unwired
// and break later BDT update/patch/delete.)
void nef_app::bdt_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info("BDT policy create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }

  oai::_3gpp::model::BdtPolicy bdt_policy;
  try {
    from_json(body, bdt_policy);
    bdt_policy.validate();
  } catch (const nlohmann::json::exception& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
            std::string("Validation failed: ") + e.what())
            .dump());
  }

  if (!bdt_policy.bdtPolDataIsSet()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            "Missing required field: bdtPolData")
            .dump());
  }
  {
    const std::string err = validate_string_param(af_id, "afId", 256);
    if (!err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              err)
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

  // FIRE: PCF BDT create (single call, phase-1). 303 = success.
  m_nef_client->create_pcf_bdt_policy_async(
      body, [this, af_id, bdt_id, bdt_policy,
             sink = std::move(sink)](oai::http::response r) mutable {
        cont_bdt_create(
            af_id, bdt_id, std::move(bdt_policy), std::move(r),
            std::move(sink));
      });
}

void nef_app::cont_bdt_create(
    const std::string& af_id, const std::string& bdt_id,
    oai::_3gpp::model::BdtPolicy bdt_policy, oai::http::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_bdt_create bdt_id=%s status=%d", bdt_id.c_str(), r.status_code);
  // SUCCESS-ON-3xx: 2xx OR 303 is success; genuine failure rolls back local
  // state + 502 (mirrors :2208-2225).
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
            http_status_code::BAD_GATEWAY, "Bad Gateway",
            "Failed to create BDT policy in PCF")
            .dump());
  }

  // §A.2b re-check: a concurrent delete may have removed bdt_id while PCF was
  // in flight. If gone, do NOT resurrect — benign 201 reflecting the local
  // state we built (the AF delete already cleaned the maps; the PCF BDT policy
  // is left for PCF/AF cleanup, matching the absence of a sync compensating
  // delete).
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

// ─── #12 bdt_policy_update — single, FATAL-502 ─────────────────────────────
// Mirrors handle_bdt_policy_update (:2243-2346): authorize/typed-parse/
// validate/owner/pcf-id-resolve block byte-identical; the PCF BDT update (sync
// :2318) becomes an async fire; FATAL-502 on failure (:2319-2326); on success
// the local store overwrite + 200 (:2329-2345) run in the continuation. §A.2b
// re-check on commit (:2331-2337 already re-checks presence).
void nef_app::bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }

  oai::_3gpp::model::BdtPolicy bdt_policy;
  try {
    from_json(body, bdt_policy);
    bdt_policy.validate();
  } catch (const nlohmann::json::exception& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            std::string("Invalid body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
            std::string("Validation failed: ") + e.what())
            .dump());
  }

  if (!bdt_policy.bdtPolDataIsSet()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            "Missing required field: bdtPolData")
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
              http_status_code::NOT_FOUND, "Not Found", "BDT policy not found")
              .dump());
    }
    auto owner_it = m_bdt_id2af_id.find(bdt_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN, "Forbidden",
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
            http_status_code::BAD_GATEWAY, "Bad Gateway",
            "Missing PCF BDT policy identifier")
            .dump());
  }
  clear_request_bearer_token();

  // FIRE: PCF BDT update (single call, phase-1).
  m_nef_client->update_pcf_bdt_policy_async(
      pcf_bdt_id, body,
      [this, af_id, bdt_id, bdt_policy,
       sink = std::move(sink)](oai::http::response r) mutable {
        cont_bdt_update(
            af_id, bdt_id, std::move(bdt_policy), std::move(r),
            std::move(sink));
      });
}

void nef_app::cont_bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    oai::_3gpp::model::BdtPolicy bdt_policy, oai::http::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_bdt_update bdt_id=%s status=%d", bdt_id.c_str(), r.status_code);
  // FATAL-502 (mirrors :2318-2326).
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

  // Commit; §A.2b re-check (mirrors :2331-2337).
  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "Not Found", "BDT policy not found")
              .dump());
    }
    session_it->second = bdt_policy;
  }
  nlohmann::json resp_json;
  to_json(resp_json, bdt_policy);
  nef_audit::log("UPDATE", "BDT", af_id, bdt_id, http_status_code::OK);
  sink(http_status_code::OK, resp_json.dump());
}

// ─── #13 bdt_policy_patch — single, FATAL-502 ──────────────────────────────
// Mirrors handle_bdt_policy_patch (:155-236): authorize/owner/pcf-id-resolve +
// merge-patch into a local patched_copy (:166-192) byte-identical; the PCF BDT
// update (sync :201) becomes an async fire; FATAL-502 on failure (:200-208); on
// success the re-parse/validate/store + 200 (:209-235) run in the continuation.
void nef_app::bdt_patch(
    const std::string& af_id, const std::string& bdt_policy_id,
    const nlohmann::json& patch_body, const std::string& token,
    response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }

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
              http_status_code::NOT_FOUND, "Not Found", "BDT policy not found")
              .dump());
    }
    auto owner_it = m_bdt_id2af_id.find(bdt_policy_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN, "Forbidden",
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
            http_status_code::BAD_GATEWAY, "Bad Gateway",
            "Missing PCF BDT policy identifier")
            .dump());
  }
  clear_request_bearer_token();

  // FIRE: PCF BDT update with the merged copy (single call, phase-1).
  m_nef_client->update_pcf_bdt_policy_async(
      pcf_bdt_id, patched_copy,
      [this, af_id, bdt_policy_id, patched_copy,
       sink = std::move(sink)](oai::http::response r) mutable {
        cont_bdt_patch(
            af_id, bdt_policy_id, std::move(patched_copy), std::move(r),
            std::move(sink));
      });
}

void nef_app::cont_bdt_patch(
    const std::string& af_id, const std::string& bdt_policy_id,
    nlohmann::json patched_copy, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_bdt_patch bdt_id=%s status=%d", bdt_policy_id.c_str(),
      r.status_code);
  // FATAL-502 (mirrors :201-207).
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

  // Commit; §A.2b re-check + re-validate the merged result (mirrors :209-231).
  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "Not Found", "BDT policy not found")
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
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              std::string("Patched body invalid: ") + e.what())
              .dump());
    }
    session_it->second = patched_policy;
  }
  patched_copy["bdtRefId"] = bdt_policy_id;
  nef_audit::log("PATCH", "BDT", af_id, bdt_policy_id, http_status_code::OK);
  sink(http_status_code::OK, patched_copy.dump());
}

// ─── #14 bdt_policy_delete — single, BEST-EFFORT (204) ─────────────────────
// Mirrors handle_bdt_policy_delete (:2349-2393): authorize/owner/pcf-id-resolve
// byte-identical; the PCF BDT delete (sync :2379, result WARN-only) becomes an
// async fire; the local erase + 204 (:2386-2392) run regardless (BEST-EFFORT).
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
    // sync skip of the southbound call, :2377).
    return cont_bdt_delete(
        af_id, bdt_id, oai::http::response{}, std::move(sink));
  }
  // FIRE: PCF BDT delete (single call, phase-1, BEST-EFFORT).
  m_nef_client->delete_pcf_bdt_policy_async(
      pcf_bdt_id, [this, af_id, bdt_id,
                   sink = std::move(sink)](oai::http::response r) mutable {
        cont_bdt_delete(af_id, bdt_id, std::move(r), std::move(sink));
      });
}

void nef_app::cont_bdt_delete(
    const std::string& af_id, const std::string& bdt_id, oai::http::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_bdt_delete bdt_id=%s status=%d", bdt_id.c_str(), r.status_code);
  // BEST-EFFORT: PCF result WARN-only (mirrors :2379-2383).
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

// ─── #15 pfd_create — single, BEST-EFFORT (201) ────────────────────────────
// Mirrors handle_pfd_create (:2054-2094): authorize/pfdDatas/path-param block
// byte-identical; the UDR PFD PUT (sync :2088, result WARN-only) becomes an
// async fire; the 201 echo body (:2091-2093) is returned regardless of the UDR
// outcome (BEST-EFFORT, :2089 WARN-only).
void nef_app::pfd_create(
    const std::string& app_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info("PFD create for app: %s", app_id.c_str());

  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }
  if (!body.contains("pfdDatas")) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            "Missing required field: pfdDatas")
            .dump());
  }
  {
    std::string err;
    if (err.empty()) err = validate_string_param(app_id, "appId", 256);
    if (err.empty()) err = validate_object_field(body, "pfdDatas", false);
    if (!err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              err)
              .dump());
    }
  }
  clear_request_bearer_token();

  // FIRE: UDR PFD PUT (single call, phase-1, BEST-EFFORT).
  m_nef_client->udr_put_pfd_data_async(
      app_id, body,
      [this, app_id, body,
       sink = std::move(sink)](oai::http::response r) mutable {
        cont_pfd_create(app_id, std::move(body), std::move(r), std::move(sink));
      });
}

void nef_app::cont_pfd_create(
    const std::string& app_id, nlohmann::json body, oai::http::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_create app_id=%s status=%d", app_id.c_str(), r.status_code);
  // BEST-EFFORT: UDR result WARN-only (mirrors :2088-2090).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn("UDR PFD push failed for app: %s", app_id.c_str());
  }
  nef_audit::log("CREATE", "PFD", app_id, app_id, http_status_code::CREATED);
  sink(http_status_code::CREATED, body.dump());
}

// ─── #16 pfd_delete — single, BEST-EFFORT (204) ────────────────────────────
// Mirrors handle_pfd_delete (:2097-2106): authorize byte-identical; the UDR
// delete (sync :2103, result unchecked) becomes an async fire; 204 (:2104-2105)
// regardless (BEST-EFFORT).
void nef_app::pfd_delete(
    const std::string& app_id, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  clear_request_bearer_token();

  // FIRE: UDR PFD delete (single call, phase-1, BEST-EFFORT).
  m_nef_client->udr_delete_pfd_data_async(
      app_id,
      [this, app_id, sink = std::move(sink)](oai::http::response r) mutable {
        cont_pfd_delete(app_id, std::move(r), std::move(sink));
      });
}

void nef_app::cont_pfd_delete(
    const std::string& app_id, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_delete app_id=%s status=%d", app_id.c_str(), r.status_code);
  // BEST-EFFORT: UDR result ignored (mirrors :2103, unchecked).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn("UDR PFD delete failed for app: %s", app_id.c_str());
  }
  nef_audit::log("DELETE", "PFD", app_id, app_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}

// ─── #17 pfd_get — single read, FATAL-propagate ────────────────────────────
// Mirrors handle_pfd_get (:2109-2140): authorize byte-identical; the UDR PFD
// GET (sync :2121) becomes an async fire; FATAL-propagate — UDR 404→AF 404 with
// "PFD data not found" (:2129-2133); 2xx→200 (:2123-2127); any other
// non-2xx→502
// (:2136-2139). Do NOT route through the generic helper (which would collapse
// the 404 to 502). NOTE: handle_pfd_get is currently unrouted (no server shim /
// no sync adapter dispatch); the split + dispatch_pfd_get_async are added for
// completeness and parity.
void nef_app::pfd_get(
    const std::string& app_id, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }
  clear_request_bearer_token();

  // FIRE: UDR PFD GET (single call, phase-1).
  m_nef_client->udr_get_pfd_data_async(
      app_id,
      [this, app_id, sink = std::move(sink)](oai::http::response r) mutable {
        cont_pfd_get(app_id, std::move(r), std::move(sink));
      });
}

void nef_app::cont_pfd_get(
    const std::string& app_id, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_get app_id=%s status=%d", app_id.c_str(), r.status_code);
  // FATAL-propagate: forward the upstream UDR status explicitly (NOT the
  // helper). The sync handle_pfd_get checks STRICT 200 (:2123, == OK), not any
  // 2xx — match exactly so a non-200 2xx falls through to the 502 branch.
  if (r.status_code == http_status_code::OK) {
    nlohmann::json result;
    try {
      result = nlohmann::json::parse(r.body);
    } catch (...) {
      result = nlohmann::json::object();
    }
    return sink(http_status_code::OK, result.dump());
  }
  if (r.status_code == http_status_code::NOT_FOUND) {
    return sink(
        http_status_code::NOT_FOUND,
        make_problem_detail(
            http_status_code::NOT_FOUND, "Not Found", "PFD data not found")
            .dump());
  }
  sink(
      http_status_code::BAD_GATEWAY,
      make_problem_detail(
          http_status_code::BAD_GATEWAY, "Bad Gateway",
          "UDR returned an unexpected response for PFD GET")
          .dump());
}

// ─── #21 pfd_app_patch — single, BEST-EFFORT (200) ─────────────────────────
// Mirrors handle_pfd_app_patch (:3635-3701): authorize/owner/app-lookup +
// merge-patch + typed re-parse + local store (:3647-3690) byte-identical; the
// UDR PFD PUT (sync :3692, result WARN-only) becomes an async fire; the 200
// echo body (:3697-3700) is returned regardless of the UDR outcome
// (BEST-EFFORT).
void nef_app::pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& patch_body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }

  nlohmann::json patched;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "Not Found",
              "PFD transaction not found")
              .dump());
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN, "Forbidden",
              "AF is not allowed to access this resource")
              .dump());
    }
    auto app_it = it->second.find(app_id);
    if (app_it == it->second.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "Not Found",
              "Application PFD not found in transaction")
              .dump());
    }
    to_json(patched, app_it->second);
    patched.merge_patch(patch_body);
    oai::_3gpp::model::PfdDataForApp patched_app;
    try {
      from_json(patched, patched_app);
      patched_app.validate();
    } catch (const std::exception& e) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              std::string("Patched body invalid: ") + e.what())
              .dump());
    }
    app_it->second = patched_app;
  }
  clear_request_bearer_token();

  // FIRE: UDR PFD PUT (single call, phase-1, BEST-EFFORT).
  m_nef_client->udr_put_pfd_data_async(
      app_id, patched,
      [this, scs_as_id, app_id, patched,
       sink = std::move(sink)](oai::http::response r) mutable {
        cont_pfd_app_patch(
            scs_as_id, app_id, std::move(patched), std::move(r),
            std::move(sink));
      });
}

void nef_app::cont_pfd_app_patch(
    const std::string& scs_as_id, const std::string& app_id,
    nlohmann::json patched, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_app_patch app_id=%s status=%d", app_id.c_str(), r.status_code);
  // BEST-EFFORT: UDR result WARN-only (mirrors :3692-3694).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app PATCH failed for app: %s", app_id.c_str());
  }
  nlohmann::json response_body = std::move(patched);
  response_body["appId"]       = app_id;
  nef_audit::log("PATCH", "PFD_APP", scs_as_id, app_id, http_status_code::OK);
  sink(http_status_code::OK, response_body.dump());
}

// ─── #22 pfd_app_delete — single, BEST-EFFORT (204) ────────────────────────
// Mirrors handle_pfd_app_delete (:3704-3732): authorize/owner/app-lookup +
// local erase (:3707-3728) byte-identical; the UDR delete (sync :3729, result
// unchecked) becomes an async fire; 204 (:3730-3731) regardless (BEST-EFFORT).
void nef_app::pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      clear_request_bearer_token();
      return sink(http_status_code::FORBIDDEN, "");
    }
    if (it->second.find(app_id) == it->second.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    it->second.erase(app_id);
  }
  clear_request_bearer_token();

  // FIRE: UDR PFD delete (single call, phase-1, BEST-EFFORT).
  m_nef_client->udr_delete_pfd_data_async(
      app_id, [this, scs_as_id, app_id,
               sink = std::move(sink)](oai::http::response r) mutable {
        cont_pfd_app_delete(scs_as_id, app_id, std::move(r), std::move(sink));
      });
}

void nef_app::cont_pfd_app_delete(
    const std::string& scs_as_id, const std::string& app_id,
    oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_app_delete app_id=%s status=%d", app_id.c_str(), r.status_code);
  // BEST-EFFORT: UDR result ignored (mirrors :3729, unchecked).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app delete failed for app: %s", app_id.c_str());
  }
  nef_audit::log(
      "DELETE", "PFD_APP", scs_as_id, app_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}

// ─── #25 nnef_pfd_put_app — single, BEST-EFFORT (201/200) ──────────────────
// Mirrors handle_nnef_pfd_put_app (:3950-4004): authorize/normalize + local
// transaction store (:3953-3990) byte-identical; the UDR PFD PUT (sync :3992,
// result WARN-only) becomes an async fire; the 201/200 echo body +
// PFD-subscriber notification (:3998-4003) run regardless of the UDR outcome
// (BEST-EFFORT).
void nef_app::nnef_pfd_put_app(
    const std::string& transaction_id, const std::string& app_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "NF not authorized for this Nnef service")
            .dump());
  }
  std::string error_detail;
  nlohmann::json normalized_app;
  if (!normalize_nnef_pfd_app_data(
          app_id, body, normalized_app, error_detail)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request", error_detail)
            .dump());
  }

  bool is_create = false;
  nlohmann::json response_app;
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto& transaction = m_nnef_pfd_transactions[transaction_id];
    if (!transaction.is_object()) {
      transaction = nlohmann::json::object();
    }
    transaction["transactionId"] = transaction_id;
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object()) {
      transaction["applications"] = nlohmann::json::object();
    }
    is_create = !transaction["applications"].contains(app_id);
    transaction["applications"][app_id] = normalized_app;
    if (transaction.contains("pfdDatas")) {
      transaction.erase("pfdDatas");
    }
    response_app = transaction["applications"][app_id];
  }
  clear_request_bearer_token();

  // FIRE: UDR PFD PUT (single call, phase-1, BEST-EFFORT).
  m_nef_client->udr_put_pfd_data_async(
      app_id, normalized_app,
      [this, app_id, response_app, normalized_app, is_create,
       sink = std::move(sink)](oai::http::response r) mutable {
        cont_nnef_pfd_put_app(
            app_id, std::move(response_app), std::move(normalized_app),
            is_create, std::move(r), std::move(sink));
      });
}

void nef_app::cont_nnef_pfd_put_app(
    const std::string& app_id, nlohmann::json response_app,
    nlohmann::json normalized_app, bool is_create, oai::http::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_nnef_pfd_put_app app_id=%s status=%d", app_id.c_str(),
      r.status_code);
  // BEST-EFFORT: UDR result WARN-only (mirrors :3992-3995).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app PUT failed for Nnef_PFDmanagement app: %s",
        app_id.c_str());
  }
  const int http_code =
      is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "NNEF_PFD_APP", "", app_id, http_code);
  // Notify SBI PFD subscribers (non-blocking; enqueues onto
  // m_notification_pool).
  notify_nnef_pfd_subscribers("PFD_CHANGE", app_id, normalized_app);
  sink(http_code, response_app.dump());
}

// ─── #26 nnef_pfd_delete_app — single, BEST-EFFORT (204) ───────────────────
// Mirrors handle_nnef_pfd_delete_app (:4007-4041): authorize/transaction-lookup
// + local erase (:4010-4030) byte-identical; the UDR delete (sync :4032, result
// WARN-only) becomes an async fire; the PFD-subscriber notify + 204
// (:4038-4040) run regardless of the UDR outcome (BEST-EFFORT).
void nef_app::nnef_pfd_delete_app(
    const std::string& transaction_id, const std::string& app_id,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto transaction_it = m_nnef_pfd_transactions.find(transaction_id);
    if (transaction_it == m_nnef_pfd_transactions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    auto& transaction = transaction_it->second;
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object() ||
        !transaction["applications"].contains(app_id)) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    transaction["applications"].erase(app_id);
  }
  clear_request_bearer_token();

  // FIRE: UDR PFD delete (single call, phase-1, BEST-EFFORT).
  m_nef_client->udr_delete_pfd_data_async(
      app_id,
      [this, app_id, sink = std::move(sink)](oai::http::response r) mutable {
        cont_nnef_pfd_delete_app(app_id, std::move(r), std::move(sink));
      });
}

void nef_app::cont_nnef_pfd_delete_app(
    const std::string& app_id, oai::http::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_nnef_pfd_delete_app app_id=%s status=%d", app_id.c_str(),
      r.status_code);
  // BEST-EFFORT: UDR result WARN-only (mirrors :4032-4035).
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app DELETE failed for Nnef_PFDmanagement app: %s",
        app_id.c_str());
  }
  // Notify SBI PFD subscribers about removal (non-blocking).
  notify_nnef_pfd_subscribers("PFD_REMOVE", app_id, nullptr);
  nef_audit::log(
      "DELETE", "NNEF_PFD_APP", "", app_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}

// ═══════════════════════════════════════════════════════════════════════════
// P4 — chained Unit 5 (6 handlers). True-async cursor pattern (§A.5/§A.5b).
// Each phase-1 (dispatcher worker) resolves the UDR (and PCF for #6) endpoint
// via discover_nf, builds a shared_ptr cursor carrying that endpoint + the work
// set + the response_sink, and kicks the cursor. Every continuation fires ONE
// discovery-free *_at_async leg and advances/rolls-back/finishes — NO thread is
// parked, NO discover_nf ever runs on oai-http-io (no io-pool self-deadlock).
// Continuations are safe to run inline on the dispatcher worker (the client's
// URI/pool sync fast-path and a wrapper's own discovery-failure cb(status 0)
// both fire the callback inline). Sync handle_* methods are unchanged.
// ═══════════════════════════════════════════════════════════════════════════

// ─── #19 pfd_transaction_delete — N-loop UDR DELETE, BEST-EFFORT 204 ─────────
// Mirrors handle_pfd_transaction_delete (:3490-3520). phase-1 does the
// authorize/owner/not-found checks + removes the transaction from local state
// under m_pfd_mutex (the snapshot of its app_ids is the delete work set,
// exactly as the sync handler erases the session then deletes each app from
// UDR), then resolves UDR and kicks the delete cursor. Per-app DELETE failures
// are ignored (idempotent, no compensation, :3515-3517); the final step sends
// 204.
void nef_app::pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  std::map<std::string, oai::_3gpp::model::PfdDataForApp> trans_body;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      clear_request_bearer_token();
      return sink(http_status_code::FORBIDDEN, "");
    }
    trans_body = it->second;
    m_pfd_trans_sessions.erase(it);
    m_pfd_trans2scs_id.erase(trans_id);
  }

  auto st  = std::make_shared<PfdDeleteChain>();
  st->sink = std::move(sink);
  for (const auto& [app_id, _] : trans_body) st->app_ids.push_back(app_id);

  // §A.1c fix (a): resolve UDR HERE (dispatcher worker), thread by value.
  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    // BEST-EFFORT: local state already erased; a discovery failure does not
    // change the AF-visible 204 (the sync per-app deletes are unchecked,
    // :3515).
    Logger::nef_app().warn(
        "PFD_TX delete: UDR discovery failed for trans_id=%s; skipping UDR "
        "deletes",
        trans_id.c_str());
    nef_audit::log(
        "DELETE", "PFD_TX", scs_as_id, trans_id, http_status_code::NO_CONTENT);
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(http_status_code::NO_CONTENT, "");
  }
  clear_request_bearer_token();

  // Audit the (already-decided) 204 here in phase-1, then kick the delete
  // cursor whose final step sends the 204 (the per-app UDR deletes are
  // best-effort and do not change the result, matching the sync handler's
  // unconditional 204).
  nef_audit::log(
      "DELETE", "PFD_TX", scs_as_id, trans_id, http_status_code::NO_CONTENT);
  pfd_transaction_delete_step(std::move(st));
}

void nef_app::pfd_transaction_delete_step(std::shared_ptr<PfdDeleteChain> st) {
  if (st->idx == st->app_ids.size()) {
    response_sink s = std::move(st->sink);
    return s(http_status_code::NO_CONTENT, "");
  }
  const std::string app_id = st->app_ids[st->idx];
  m_nef_client->udr_delete_pfd_data_at_async(
      st->udr_ep, app_id, [this, st, app_id](oai::http::response r) mutable {
        // BEST-EFFORT, idempotent: failures are ignored (no compensation),
        // matching the sync per-app delete whose result is unchecked (:3515).
        if (!sbi_ok(r)) {
          Logger::nef_app().warn(
              "PFD_TX delete: UDR PFD delete failed for app=%s (http=%d)",
              app_id.c_str(), r.status_code);
        }
        st->idx++;
        pfd_transaction_delete_step(std::move(st));
      });
}

// ─── #24 nnef_pfd_delete_transaction — N-loop UDR DELETE, BEST-EFFORT 204
// ───── Mirrors handle_nnef_pfd_delete_transaction (:3880-3911). Same delete
// cursor as #19 (reuses PfdDeleteChain), no compensation. phase-1 removes the
// transaction from m_nnef_pfd_transactions and snapshots its application
// app_ids.
void nef_app::nnef_pfd_delete_transaction(
    const std::string& transaction_id, const std::string& token,
    response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  nlohmann::json transaction;
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto it = m_nnef_pfd_transactions.find(transaction_id);
    if (it == m_nnef_pfd_transactions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    transaction = it->second;
    m_nnef_pfd_transactions.erase(it);
  }

  auto st  = std::make_shared<PfdDeleteChain>();
  st->sink = std::move(sink);
  if (transaction.contains("applications") &&
      transaction["applications"].is_object()) {
    for (const auto& [app_id, _] : transaction["applications"].items())
      st->app_ids.push_back(app_id);
  }

  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    Logger::nef_app().warn(
        "NNEF_PFD_TX delete: UDR discovery failed for trans_id=%s; skipping "
        "UDR "
        "deletes",
        transaction_id.c_str());
    nef_audit::log(
        "DELETE", "NNEF_PFD_TX", "", transaction_id,
        http_status_code::NO_CONTENT);
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(http_status_code::NO_CONTENT, "");
  }
  clear_request_bearer_token();

  nef_audit::log(
      "DELETE", "NNEF_PFD_TX", "", transaction_id,
      http_status_code::NO_CONTENT);
  nnef_pfd_delete_transaction_step(std::move(st));
}

void nef_app::nnef_pfd_delete_transaction_step(
    std::shared_ptr<PfdDeleteChain> st) {
  if (st->idx == st->app_ids.size()) {
    response_sink s = std::move(st->sink);
    return s(http_status_code::NO_CONTENT, "");
  }
  const std::string app_id = st->app_ids[st->idx];
  m_nef_client->udr_delete_pfd_data_at_async(
      st->udr_ep, app_id, [this, st, app_id](oai::http::response r) mutable {
        // BEST-EFFORT: WARN-only on failure (mirrors :3902-3907).
        if (!sbi_ok(r)) {
          Logger::nef_app().warn(
              "UDR PFD delete failed for Nnef_PFDmanagement app: %s (http=%d)",
              app_id.c_str(), r.status_code);
        }
        st->idx++;
        nnef_pfd_delete_transaction_step(std::move(st));
      });
}

// ─── #27 nnef_pfd_partial_pull — N-loop UDR GET (reads), BEST-EFFORT ─────────
// Mirrors handle_nnef_pfd_partial_pull (:4083-4129). §A.5
// SNAPSHOT-then-release: the sync handler holds
// shared_lock(m_nnef_pfd_transactions_mutex) across all N GETs — a lock CANNOT
// span async hops. phase-1 SNAPSHOTS the iteration set under the lock (applying
// the requested_ids filter, copying {app_id, fallback}=stored app_data per
// :4119-4123), RELEASES the lock, then runs the read cursor. Each step fires
// udr_get_pfd_data_at_async; the continuation pushes (sbi_ok ? parsed(r) :
// fallback) with ["applicationId"]=app_id; the final step sends 200 with the
// array. No nef_app lock is held across any async hop.
void nef_app::nnef_pfd_partial_pull(
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "NF not authorized for this Nnef service")
            .dump());
  }

  std::vector<std::string> requested_ids;
  if (body.contains("appIds") && body["appIds"].is_array()) {
    for (const auto& v : body["appIds"]) {
      if (v.is_string()) requested_ids.push_back(v.get<std::string>());
    }
  }

  auto st  = std::make_shared<PfdPullChain>();
  st->sink = std::move(sink);
  // SNAPSHOT the iteration set under the lock, then RELEASE before any GET.
  {
    std::shared_lock lock(m_nnef_pfd_transactions_mutex);
    for (const auto& [trans_id, transaction] : m_nnef_pfd_transactions) {
      if (!transaction.contains("applications") ||
          !transaction["applications"].is_object())
        continue;
      for (const auto& [app_id, app_data] :
           transaction["applications"].items()) {
        if (!requested_ids.empty()) {
          bool found = false;
          for (const auto& f : requested_ids)
            if (f == app_id) {
              found = true;
              break;
            }
          if (!found) continue;
        }
        st->apps.emplace_back(app_id, app_data);  // fallback = stored app_data
      }
    }
  }  // lock released here — the GET cursor runs lock-free

  if (st->apps.empty()) {
    // Nothing to query — return the empty array immediately (no southbound).
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(http_status_code::OK, st->result.dump());
  }

  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    // BEST-EFFORT: UDR refresh impossible → fall back to the stored app_data
    // for every entry (exactly the sync :4123 fallback when the UDR read
    // fails).
    Logger::nef_app().warn(
        "NNEF_PFD partial-pull: UDR discovery failed; returning cached PFD "
        "data");
    for (auto& [app_id, fallback] : st->apps) {
      nlohmann::json entry   = fallback;
      entry["applicationId"] = app_id;
      st->result.push_back(entry);
    }
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(http_status_code::OK, st->result.dump());
  }
  clear_request_bearer_token();

  nnef_pfd_partial_pull_step(std::move(st));
}

void nef_app::nnef_pfd_partial_pull_step(std::shared_ptr<PfdPullChain> st) {
  if (st->idx == st->apps.size()) {
    response_sink s = std::move(st->sink);
    return s(http_status_code::OK, st->result.dump());
  }
  const std::string app_id      = st->apps[st->idx].first;
  const nlohmann::json fallback = st->apps[st->idx].second;
  m_nef_client->udr_get_pfd_data_at_async(
      st->udr_ep, app_id,
      [this, st, app_id, fallback](oai::http::response r) mutable {
        // BEST-EFFORT per-app, byte-parity with the sync path (:4119-4123):
        // entry = (udr_code == OK) ? udr_result : app_data, where the sync
        // udr_get_pfd_data sets udr_result to the parsed body on 200 (and to an
        // empty object {} if the 200 body is empty/unparseable,
        // :1432/:1463-1469; it strictly compares == OK, not 2xx). Reproduce
        // that exactly.
        nlohmann::json entry;
        if (r.status_code == http_status_code::OK) {
          entry = nlohmann::json::object();  // matches sync result default
          if (!r.body.empty()) {
            try {
              entry = nlohmann::json::parse(r.body);
            } catch (...) {
              Logger::nef_app().warn(
                  "Failed to parse UDR PFD GET response body for app=%s",
                  app_id.c_str());
              entry = nlohmann::json::object();
            }
          }
        } else {
          Logger::nef_app().debug(
              "NNEF_PFD partial-pull: using cached PFD data for app=%s "
              "(http=%d)",
              app_id.c_str(), r.status_code);
          entry = fallback;
        }
        entry["applicationId"] = app_id;
        st->result.push_back(entry);
        st->idx++;
        nnef_pfd_partial_pull_step(std::move(st));
      });
}

// ─── #18 pfd_transaction_put — N-loop UDR PUT + compensating rollback
// ───────── FATAL-500. Mirrors handle_pfd_transaction_put (:3382-3487). phase-1
// does the authorize/validate/typed-parse of pfdDatas + create-vs-update
// determination + UDR discovery, then the PfdPutChain cursor sequentially PUTs
// each app. On any step-k failure pfd_put_rollback issues compensating DELETEs
// over committed[0.. k-1] in REVERSE, then sends 500 from the last rollback
// continuation (mirrors the PfdRollbackTracker, :3441-3469). On full success
// the cursor commits local state and sends 201 (create) / 200 (update)
// (:3474-3486). Two-phase ONLY.
void nef_app::pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "AF not authorized for this service")
            .dump());
  }
  if (!body.contains("pfdDatas")) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request",
            "Missing required field: pfdDatas")
            .dump());
  }
  if (!body["pfdDatas"].is_object()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
            "pfdDatas: must be an object")
            .dump());
  }

  auto st       = std::make_shared<PfdPutChain>();
  st->scs_as_id = scs_as_id;
  st->trans_id  = trans_id;
  st->body      = body;
  st->sink      = std::move(sink);

  // Typed parse + validate each app's PFD data (mirrors :3409-3430).
  for (auto& [app_id, app_json] : body["pfdDatas"].items()) {
    oai::_3gpp::model::PfdDataForApp app;
    try {
      from_json(app_json, app);
      app.validate();
    } catch (const nlohmann::json::exception& e) {
      response_sink s = std::move(st->sink);
      clear_request_bearer_token();
      return s(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST, "Bad Request",
              "pfdDatas." + app_id + ": " + e.what())
              .dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      response_sink s = std::move(st->sink);
      clear_request_bearer_token();
      return s(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
              "pfdDatas." + app_id + ": " + e.what())
              .dump());
    }
    nlohmann::json pfd_json;
    to_json(pfd_json, app);
    st->apps.emplace_back(app_id, std::move(pfd_json));
    st->app_map[app_id] = std::move(app);
  }

  // Determine create-vs-update before UDR writes (deferred local commit,
  // :3433).
  {
    std::shared_lock lock(m_pfd_mutex);
    st->is_create =
        (m_pfd_trans_sessions.find(trans_id) == m_pfd_trans_sessions.end());
  }

  // §A.1c fix (a): resolve UDR HERE on the dispatcher worker.
  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    // No UDR write happened yet → nothing to roll back. FATAL-500 (the sync
    // path aborts the transaction with 500 on a UDR write failure, :3465).
    Logger::nef_app().error(
        "PFD_TX put: UDR discovery failed for trans_id=%s", trans_id.c_str());
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(
        http_status_code::INTERNAL_SERVER_ERROR,
        make_problem_detail(
            http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
            "PFD transaction aborted: UDR not available")
            .dump());
  }
  clear_request_bearer_token();

  pfd_put_step(std::move(st));
}

void nef_app::pfd_put_step(std::shared_ptr<PfdPutChain> st) {
  if (st->idx == st->apps.size()) {
    // All UDR writes succeeded — commit local state (deferred commit, :3474).
    {
      const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
      m_pfd_trans_sessions[st->trans_id] = std::move(st->app_map);
      m_pfd_trans2scs_id[st->trans_id]   = st->scs_as_id;
    }
    nlohmann::json resp = st->body;
    resp["transId"]     = st->trans_id;
    const int code =
        st->is_create ? http_status_code::CREATED : http_status_code::OK;
    nef_audit::log(
        st->is_create ? "CREATE" : "UPDATE", "PFD_TX", st->scs_as_id,
        st->trans_id, code);
    response_sink s = std::move(st->sink);
    return s(code, resp.dump());
  }
  const std::string app_id       = st->apps[st->idx].first;
  const nlohmann::json& pfd_json = st->apps[st->idx].second;
  m_nef_client->udr_put_pfd_data_at_async(
      st->udr_ep, app_id, pfd_json,
      [this, st, app_id](oai::http::response r) mutable {
        if (!sbi_ok(r)) {
          Logger::nef_app().error(
              "F1.10: UDR PFD write failed for app '%s' in trans '%s'; rolling "
              "back %zu committed app(s)",
              app_id.c_str(), st->trans_id.c_str(), st->committed.size());
          return pfd_put_rollback(std::move(st), app_id);
        }
        st->committed.push_back(app_id);
        st->idx++;
        pfd_put_step(std::move(st));
      });
}

void nef_app::pfd_put_rollback(
    std::shared_ptr<PfdPutChain> st, const std::string& failed_app) {
  // Async compensating DELETEs over committed[0..k-1] in REVERSE; the last
  // rollback continuation sends 500 (mirrors PfdRollbackTracker::execute +
  // :3465-3469). No local state was committed yet, so rollback is southbound
  // only.
  pfd_rollback_step(std::move(st), st->committed.size(), failed_app);
}

void nef_app::pfd_rollback_step(
    std::shared_ptr<PfdPutChain> st, std::size_t remaining,
    const std::string& failed_app) {
  if (remaining == 0) {
    // Rollback complete — abort the transaction with 500.
    nlohmann::json pd = make_problem_detail(
        http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
        "PFD transaction aborted: UDR write failed for app " + failed_app);
    response_sink s = std::move(st->sink);
    return s(http_status_code::INTERNAL_SERVER_ERROR, pd.dump());
  }
  const std::string rid = st->committed[remaining - 1];  // reverse order
  m_nef_client->udr_delete_pfd_data_at_async(
      st->udr_ep, rid,
      [this, st, remaining, failed_app, rid](oai::http::response r) mutable {
        if (!sbi_ok(r)) {
          // Mirrors the rollback-delete failure log (:3454-3463); UDR may
          // retain orphan data, but the request still aborts with 500.
          Logger::nef_app().error(
              "F1.10: Rollback delete failed for app '%s' in trans '%s'",
              rid.c_str(), st->trans_id.c_str());
        }
        pfd_rollback_step(std::move(st), remaining - 1, failed_app);
      });
}

// ─── #6 traffic_influence_delete — chained PCF→UDR (both deletes) ────────────
// BEST-EFFORT 204. Mirrors handle_traffic_influence_delete (:1992-2050).
// phase-1 does authorize/not-found/owner checks, captures the PCF policy id,
// and resolves PCF+UDR endpoints. cont_ti_delete_pcf fires the PCF app-session
// DELETE (only when a PCF policy exists, :2020), then cont_ti_delete_udr fires
// the UDR influence DELETE; both failures are WARN-only (:2023/:2031) and the
// response is always 204. Local state is erased in the FINAL continuation
// (after both SBI calls), preserving the sync ordering (:2036-2047).
// Idempotent, no compensation. Empty sink.
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

  // §A.1c fix (a): resolve BOTH NF endpoints HERE (dispatcher worker).
  std::string pcf_ep, udr_ep;
  const bool pcf_ok = pcf_policy_id.empty() ||
                      m_nef_client->discover_nf(nf_type_t::NF_TYPE_PCF, pcf_ep);
  const bool udr_ok = m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, udr_ep);
  clear_request_bearer_token();

  if (pcf_policy_id.empty() || !pcf_ok) {
    // No PCF policy to delete (or PCF undiscoverable) — skip the PCF leg, go
    // straight to the UDR leg. A PCF discovery failure is WARN-only
    // (BEST-EFFORT, mirrors the sync PCF-failure-is-WARN-only branch, :2023).
    if (!pcf_policy_id.empty() && !pcf_ok) {
      Logger::nef_app().warn(
          "PCF TI delete: PCF discovery failed for ti_id=%s policy_id=%s",
          ti_id.c_str(), pcf_policy_id.c_str());
    }
    // Inline-finish the PCF leg with a status-0 sentinel (no southbound fire),
    // then the UDR leg runs from cont_ti_delete_pcf's pass-through.
    return cont_ti_delete_pcf(
        af_id, ti_id, udr_ok ? udr_ep : std::string{}, oai::http::response{},
        std::move(sink));
  }

  // FIRE leg #1: PCF app-session DELETE via the discovery-free *_at_async.
  m_nef_client->delete_pcf_policy_auth_at_async(
      pcf_ep, pcf_policy_id,
      [this, af_id, ti_id, ti_policy = pcf_policy_id,
       udr_ep = (udr_ok ? udr_ep : std::string{}),
       sink   = std::move(sink)](oai::http::response r) mutable {
        if (!sbi_ok(r)) {
          Logger::nef_app().warn(
              "PCF TI delete failed for ti_id=%s policy_id=%s (http=%d)",
              ti_id.c_str(), ti_policy.c_str(), r.status_code);
        }
        cont_ti_delete_pcf(af_id, ti_id, udr_ep, std::move(r), std::move(sink));
      });
}

void nef_app::cont_ti_delete_pcf(
    const std::string& af_id, const std::string& ti_id,
    const std::string& udr_ep, oai::http::response /*r*/, response_sink sink) {
  // PCF leg already logged (or skipped) by the caller. Now fire the UDR
  // influence DELETE. If UDR was undiscoverable (empty udr_ep) skip it
  // (BEST-EFFORT, WARN-only, mirrors :2031) and go straight to the final step.
  if (udr_ep.empty()) {
    Logger::nef_app().warn(
        "UDR influence DELETE skipped for ti_id=%s (UDR undiscoverable)",
        ti_id.c_str());
    return cont_ti_delete_udr(
        af_id, ti_id, oai::http::response{}, std::move(sink));
  }
  m_nef_client->udr_delete_influence_data_at_async(
      udr_ep, ti_id,
      [this, af_id, ti_id,
       sink = std::move(sink)](oai::http::response ur) mutable {
        cont_ti_delete_udr(af_id, ti_id, std::move(ur), std::move(sink));
      });
}

void nef_app::cont_ti_delete_udr(
    const std::string& af_id, const std::string& ti_id, oai::http::response r,
    response_sink sink) {
  // BEST-EFFORT: UDR failure is WARN-only and does not change the 204 (mirrors
  // :2030-2034). r.status_code==0 here means the UDR leg was skipped (already
  // logged by the caller) — do not emit a spurious second WARN in that case.
  if (r.status_code != 0 && !sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR influence DELETE failed for ti_id=%s (http=%d)", ti_id.c_str(),
        r.status_code);
  }

  // Erase local state AFTER both SBI calls (preserves sync ordering,
  // :2036-2047). Recover the PCF policy id under the lock to clean
  // m_nf2af_sub_id.
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

// ─── #23 nnef_pfd_put_transaction — THREE-PHASE (§A.5b) ──────────────────────
// Mirrors handle_nnef_pfd_put_transaction (:3753-3854). ≠ #18 (row 18 is
// two-phase). Phase A = N-loop UDR PUT (NnefPutChain); Phase B = compensating
// DELETE rollback on PUT failure → 500 (FATAL-500, :3811-3815); Phase C =
// POST-COMMIT best-effort removed-app cleanup: after the PUT loop succeeds AND
// local state is committed (:3820-3825), SEND the success response (201/200,
// :3841-3845), THEN fire fire-and-forget udr_delete_pfd_data_at_async for each
// app in removed_apps (prior-transaction apps absent from the new set, computed
// in phase-1 under the mutex, :3827-3831). Phase-C continuations capture ONLY
// value copies (tid, app_id) — NOT st, NOT the sink — WARN-only on failure
// (:3833), NEVER re-touch the spent deferred; success is NOT gated on phase C.
void nef_app::nnef_pfd_put_transaction(
    const std::string& transaction_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN, "Forbidden",
            "NF not authorized for this Nnef service")
            .dump());
  }
  nlohmann::json applications;
  std::string error_detail;
  if (!extract_nnef_pfd_transaction_apps(body, applications, error_detail)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Bad Request", error_detail)
            .dump());
  }

  auto st            = std::make_shared<NnefPutChain>();
  st->transaction_id = transaction_id;
  st->applications   = applications;
  st->transaction =
      make_nnef_pfd_transaction(transaction_id, body, applications);
  st->sink = std::move(sink);
  for (const auto& [app_id, _] : applications.items())
    st->app_ids.push_back(app_id);

  // Determine create-vs-update + compute the phase-C removed-app set under the
  // mutex (prior transaction apps absent from the new set,
  // :3776-3785/:3827-3831).
  {
    std::shared_lock lock(m_nnef_pfd_transactions_mutex);
    auto it       = m_nnef_pfd_transactions.find(transaction_id);
    st->is_create = (it == m_nnef_pfd_transactions.end());
    if (!st->is_create) {
      const nlohmann::json& prev = it->second;
      if (prev.contains("applications") && prev["applications"].is_object()) {
        for (const auto& [app_id, _] : prev["applications"].items()) {
          if (!applications.contains(app_id))
            st->removed_apps.push_back(app_id);
        }
      }
    }
  }

  // §A.1c fix (a): resolve UDR HERE (dispatcher worker).
  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    // No UDR write happened yet → nothing to roll back. FATAL-500 (:3811).
    Logger::nef_app().error(
        "NNEF_PFD_TX put: UDR discovery failed for trans_id=%s",
        transaction_id.c_str());
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(
        http_status_code::INTERNAL_SERVER_ERROR,
        make_problem_detail(
            http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
            "PFD transaction aborted: UDR not available")
            .dump());
  }
  clear_request_bearer_token();

  nnef_put_step(std::move(st));
}

// Phase A step: PUT each app; on failure → phase B (nnef_put_rollback, 500);
// when all apps PUT → nnef_put_after_commit (commit + success + phase C).
void nef_app::nnef_put_step(std::shared_ptr<NnefPutChain> st) {
  if (st->idx == st->app_ids.size()) {
    return nnef_put_after_commit(std::move(st));
  }
  const std::string app_id      = st->app_ids[st->idx];
  const nlohmann::json app_body = st->applications[app_id];
  m_nef_client->udr_put_pfd_data_at_async(
      st->udr_ep, app_id, app_body,
      [this, st, app_id](oai::http::response r) mutable {
        if (!sbi_ok(r)) {
          Logger::nef_app().error(
              "UDR PFD write failed for Nnef app '%s' in trans '%s'; rolling "
              "back %zu committed app(s)",
              app_id.c_str(), st->transaction_id.c_str(), st->committed.size());
          return nnef_put_rollback(std::move(st), app_id);
        }
        st->committed.push_back(app_id);
        st->idx++;
        nnef_put_step(std::move(st));
      });
}

// Phase B: compensating DELETEs over committed[0..k-1] in REVERSE → 500
// (FATAL-500, mirrors PfdRollbackTracker + :3811-3815). Southbound only — no
// local state was committed yet.
void nef_app::nnef_put_rollback(
    std::shared_ptr<NnefPutChain> st, const std::string& failed_app) {
  nnef_rollback_step(std::move(st), st->committed.size(), failed_app);
}

void nef_app::nnef_rollback_step(
    std::shared_ptr<NnefPutChain> st, std::size_t remaining,
    const std::string& failed_app) {
  if (remaining == 0) {
    nlohmann::json pd = make_problem_detail(
        http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
        "PFD transaction aborted: UDR write failed for app " + failed_app);
    response_sink s = std::move(st->sink);
    return s(http_status_code::INTERNAL_SERVER_ERROR, pd.dump());
  }
  const std::string rid = st->committed[remaining - 1];  // reverse order
  m_nef_client->udr_delete_pfd_data_at_async(
      st->udr_ep, rid,
      [this, st, remaining, failed_app, rid](oai::http::response r) mutable {
        if (!sbi_ok(r)) {
          Logger::nef_app().error(
              "Rollback delete failed for Nnef app '%s' in trans '%s'",
              rid.c_str(), st->transaction_id.c_str());
        }
        nnef_rollback_step(std::move(st), remaining - 1, failed_app);
      });
}

// Phase A done → commit local state, SEND the success response, then phase C.
void nef_app::nnef_put_after_commit(std::shared_ptr<NnefPutChain> st) {
  // Commit local state (:3820-3825).
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    m_nnef_pfd_transactions[st->transaction_id] = st->transaction;
  }

  const int code =
      st->is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      st->is_create ? "CREATE" : "UPDATE", "NNEF_PFD_TX", "",
      st->transaction_id, code);

  // SEND THE SUCCESS RESPONSE NOW — phase C does NOT gate it (mirrors
  // :3841-3845). After this move the sink is spent (exactly-once); phase C must
  // not and cannot complete the handle again.
  st->sink(code, st->transaction.dump());

  // Notify SBI PFD subscribers (non-blocking on m_notification_pool,
  // :3847-3853).
  if (st->transaction.contains("applications") &&
      st->transaction["applications"].is_object()) {
    for (const auto& [app_id, app_data] :
         st->transaction["applications"].items()) {
      notify_nnef_pfd_subscribers("PFD_CHANGE", app_id, app_data);
    }
  }

  // Phase C — POST-COMMIT best-effort removed-app cleanup (:3827-3838). Fire-
  // and-forget async DELETEs; failures are WARN-only and ignored; NO rollback;
  // response already sent. Continuations capture ONLY value copies (tid,
  // app_id) — NOT st, NOT the sink — so the spent deferred handle is never
  // re-touched.
  const std::string udr_ep = st->udr_ep;  // copy out before st is released
  const std::string tid    = st->transaction_id;
  for (const std::string& app_id : st->removed_apps) {  // empty on create
    m_nef_client->udr_delete_pfd_data_at_async(
        udr_ep, app_id, [tid, app_id](oai::http::response r) {
          if (!(r.status_code >= 200 && r.status_code < 300)) {
            Logger::nef_app().warn(
                "UDR PFD delete failed for removed Nnef_PFDmanagement app: %s "
                "in transaction: %s",
                app_id.c_str(), tid.c_str());
          }
        });
  }
  // st (and its now-spent sink) drops when this frame returns.
}
