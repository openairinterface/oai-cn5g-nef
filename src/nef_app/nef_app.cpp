/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_app.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/bind/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <chrono>
#include <ctime>
#include <sstream>
#include <thread>

#include "3gpp_29.500.h"
#include "logger.hpp"
#include "nef_audit_log.hpp"
#include "nef_callback_uri_validator.hpp"
#include "nef_input_validation.hpp"
#include "nef_pfd_atomicity.hpp"
#include "nef_pfd_management_quality.hpp"
#include "nef_client.hpp"
#include "nef_config.hpp"
#include "nef_config_types.hpp"
#include "nef_json_utils.hpp"
#include "nef_jwt.hpp"
#include "nef_notification_mapper.hpp"

#include <algorithm>

using namespace oai::nef::app;
using namespace boost::placeholders;
using namespace oai::common::sbi;

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

static nlohmann::json make_problem_detail(
    int status, const std::string& title, const std::string& detail,
    const std::string& instance = "");

// Bridge helpers for nlohmann ↔ rfl::Generic conversion
static inline rfl::Generic j2g(const nlohmann::json& j) {
  auto r = rfl::json::read<rfl::Generic>(j.dump());
  return r ? r.value() : rfl::Generic(rfl::Generic::Object{});
}
static inline nlohmann::json g2j(const rfl::Generic& g) {
  return nlohmann::json::parse(rfl::json::write(g));
}

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
    const std::string& scs_as_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }
  if (!body.contains("analyEventsSubs")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing analyEventsSubs in request body");
    rfl_response = j2g(response_body);
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
  http_code    = http_status_code::OK;
  rfl_response = j2g(response_body);
}

// BDT PATCH
//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_patch(
    const std::string& af_id, const std::string& bdt_policy_id,
    const rfl::Generic& patch_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
      return;
    }
    auto owner_it = m_bdt_id2af_id.find(bdt_policy_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      rfl_response = j2g(response_body);
      return;
    }
    auto pcf_it = m_bdt_id2pcf_policy_id.find(bdt_policy_id);
    if (pcf_it != m_bdt_id2pcf_policy_id.end()) {
      pcf_bdt_id = pcf_it->second;
    }
    auto r_base = rfl::json::read<rfl::Generic>(session_it->second.dump());
    if (!r_base) {
      http_code     = http_status_code::INTERNAL_SERVER_ERROR;
      response_body = make_problem_detail(
          http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
          "Failed to parse stored BDT session");
      rfl_response = j2g(response_body);
      return;
    }
    const rfl::Generic r_result = nef_merge_patch(r_base.value(), patch_body);
    patched_copy = nlohmann::json::parse(rfl::json::write(r_result));
  }
  if (pcf_bdt_id.empty()) {
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF BDT policy identifier");
    rfl_response = j2g(response_body);
    return;
  }
  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_bdt_policy(
          pcf_bdt_id, patched_copy, http_code_pcf, http_version)) {
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update BDT policy in PCF");
    rfl_response = j2g(response_body);
    return;
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      rfl_response = j2g(response_body);
      return;
    }
    session_it->second = patched_copy;
  }
  response_body             = patched_copy;
  response_body["bdtRefId"] = bdt_policy_id;
  http_code                 = http_status_code::OK;
  rfl_response              = j2g(response_body);
  nef_audit::log("PATCH", "BDT", af_id, bdt_policy_id, http_code);
}

// QoS UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "QoS subscription not found");
    rfl_response = j2g(response_body);
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    rfl_response = j2g(response_body);
    return;
  }
  // Type and length validation (422 for semantic errors).
  {
    const std::string err =
        validate_string_field(body, "notifUri", false, 2048);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      rfl_response = j2g(response_body);
      return;
    }
  }
  // SSRF protection: validate callback URI before updating stored state
  if (body.contains("notifUri") && body["notifUri"].is_string()) {
    const std::string uri_err =
        validate_callback_uri(body["notifUri"].get<std::string>());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
      rfl_response = j2g(response_body);
      return;
    }
  }
  sub->set_subscription_data(body);
  if (body.contains("notifUri") && body["notifUri"].is_string()) {
    sub->set_notification_uri(body["notifUri"].get<std::string>());
  }
  // Optionally: re-subscribe to SMF if needed (not implemented here)
  response_body          = sub->get_subscription_data();
  response_body["subId"] = sub_id;
  http_code              = http_status_code::OK;
  nef_audit::log("UPDATE", "QOS", scs_as_id, sub_id, http_code);
  rfl_response = j2g(response_body);
}

// Monitoring Event UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "Subscription not found");
    rfl_response = j2g(response_body);
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    rfl_response = j2g(response_body);
    return;
  }
  // SSRF protection: validate callback URI before updating stored state
  if (body.contains("notificationDestination") &&
      body["notificationDestination"].is_string()) {
    const std::string uri_err = validate_callback_uri(
        body["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "notificationDestination: " + uri_err);
      rfl_response = j2g(response_body);
      return;
    }
  }
  sub->set_subscription_data(body);
  if (body.contains("notificationDestination")) {
    sub->set_notification_uri(
        body["notificationDestination"].get<std::string>());
  }
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
  rfl_response = j2g(response_body);
}

// TI GET
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_get(
    const std::string& af_id, const std::string& app_session_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }
  std::shared_lock lock(m_ti_mutex);
  auto it = m_ti_sessions.find(app_session_id);
  if (it == m_ti_sessions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "TI session not found");
    rfl_response = j2g(response_body);
    return;
  }
  auto owner_it = m_ti_id2af_id.find(app_session_id);
  if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this resource");
    rfl_response = j2g(response_body);
    return;
  }
  response_body              = it->second;
  response_body["afTransId"] = app_session_id;
  http_code                  = http_status_code::OK;
  rfl_response               = j2g(response_body);
}

// TI LIST
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_list(
    const std::string& af_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
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
  http_code    = http_status_code::OK;
  rfl_response = j2g(response_body);
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
  if (!body.contains("eventsSubs") || !body["eventsSubs"].is_array() ||
      body["eventsSubs"].empty()) {
    error_detail = "eventsSubs is required and must be a non-empty array";
    return false;
  }

  // TS 29.591 §5.4.2: each NefEventSubs item must have an "event" field.
  std::size_t idx = 0;
  for (const auto& item : body["eventsSubs"]) {
    if (!item.is_object() || !item.contains("event") ||
        !item["event"].is_string() ||
        item["event"].get<std::string>().empty()) {
      error_detail = "eventsSubs[" + std::to_string(idx) +
                     "].event: required non-empty string";
      return false;
    }
    ++idx;
  }

  if (!body.contains("notifUri") || !body["notifUri"].is_string() ||
      body["notifUri"].get<std::string>().empty()) {
    error_detail = "notifUri is required and must be a non-empty string";
    return false;
  }

  if (!body.contains("notifId") || !body["notifId"].is_string() ||
      body["notifId"].get<std::string>().empty()) {
    error_detail = "notifId is required and must be a non-empty string";
    return false;
  }

  error_detail = validate_callback_uri(body["notifUri"].get<std::string>());
  if (!error_detail.empty()) {
    error_detail = "notifUri: " + error_detail;
    return false;
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
void nef_app::handle_nf_notification(
    const std::string& nf_sub_id, const nlohmann::json& notif_payload) {
  Logger::nef_app().debug(
      "Received NF notification for NF-sub-id: %s", nf_sub_id.c_str());

  // Map NF sub-id → AF sub-id
  std::string af_sub_id;
  {
    std::shared_lock lock(m_nf2af_mutex);
    auto it = m_nf2af_sub_id.find(nf_sub_id);
    if (it == m_nf2af_sub_id.end()) {
      Logger::nef_app().warn(
          "No AF subscription found for NF sub-id: %s", nf_sub_id.c_str());
      return;
    }
    af_sub_id = it->second;
  }

  // Find the AF subscription
  auto sub = find_subscription(af_sub_id);
  if (!sub) {
    Logger::nef_app().warn("AF subscription %s not found", af_sub_id.c_str());
    return;
  }

  // Forward to AF — translate southbound → northbound format via mapper
  std::string af_uri = sub->get_notification_uri();
  if (!af_uri.empty()) {
    rfl::Generic rfl_notif = j2g(notif_payload);
    rfl::Generic rfl_t8;
    bool mapped = false;
    auto svc    = sub->get_service_type();
    if (svc == nef_service_type_t::NEF_SERVICE_TYPE_MONITORING_EVENT) {
      mapped = nef_notification_mapper::amf_to_monitoring_notification(
          rfl_notif, rfl_t8, af_sub_id);
    } else if (svc == nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING) {
      mapped = nef_notification_mapper::smf_to_qos_notification(
          rfl_notif, rfl_t8, af_sub_id);
    } else if (svc == nef_service_type_t::NEF_SERVICE_TYPE_TRAFFIC_INFLUENCE) {
      mapped = nef_notification_mapper::pcf_to_ti_notification(
          rfl_notif, rfl_t8, af_sub_id);
    } else {
      // Pass-through for other service types (Analytics, PFD, BDT, etc.)
      rfl_t8 = rfl_notif;
      mapped = true;
    }

    if (!mapped) {
      Logger::nef_app().warn(
          "Notification mapping failed for sub %s – forwarding raw payload",
          af_sub_id.c_str());
      rfl_t8 = rfl_notif;
    }

    const uint8_t http_ver       = sub->get_http_version();
    auto nef_client              = m_nef_client;
    const nlohmann::json t8_json = g2j(rfl_t8);
    const bool enqueued = m_notification_pool->enqueue([nef_client, af_uri,
                                                        t8_json, http_ver]() {
      if (!nef_client->forward_notification_to_af(af_uri, t8_json, http_ver)) {
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
}

// Nnef_EventExposure (TS 29.591)
//------------------------------------------------------------------------------
void nef_app::handle_nnef_event_exposure_subscribe(
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  (void) http_version;

  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }

  std::string error_detail;
  if (!validate_nnef_event_exposure_subscription(body, error_detail)) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    rfl_response = j2g(response_body);
    return;
  }

  std::string subscription_id;
  generate_af_subscription_id(subscription_id);

  nlohmann::json stored_subscription = body;
  finalize_nnef_event_exposure_subscription(
      stored_subscription, subscription_id);

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_event_subscriptions_mutex);
    m_nnef_event_subscriptions[subscription_id] = stored_subscription;
  }

  Logger::nef_app().info(
      "Created Nnef_EventExposure subscription: %s", subscription_id.c_str());

  response_body = stored_subscription;
  http_code     = http_status_code::CREATED;
  nef_audit::log("CREATE", "EE", "", subscription_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id, int& http_code, uint8_t http_version) {
  (void) http_version;

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
    const std::string& subscription_id, rfl::Generic& rfl_response,
    int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  (void) http_version;

  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }

  std::shared_lock lock(m_nnef_event_subscriptions_mutex);
  auto it = m_nnef_event_subscriptions.find(subscription_id);
  if (it == m_nnef_event_subscriptions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Nnef_EventExposure subscription not found");
    rfl_response = j2g(response_body);
    return;
  }

  response_body = it->second;
  http_code     = http_status_code::OK;
  rfl_response  = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_event_exposure_update(
    const std::string& subscription_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  (void) http_version;

  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }

  std::string error_detail;
  if (!validate_nnef_event_exposure_subscription(body, error_detail)) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    rfl_response = j2g(response_body);
    return;
  }

  nlohmann::json updated_subscription = body;
  finalize_nnef_event_exposure_subscription(
      updated_subscription, subscription_id);

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_event_subscriptions_mutex);
    auto it = m_nnef_event_subscriptions.find(subscription_id);
    if (it == m_nnef_event_subscriptions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "Nnef_EventExposure subscription not found");
      rfl_response = j2g(response_body);
      return;
    }

    it->second = updated_subscription;
  }

  Logger::nef_app().info(
      "Updated Nnef_EventExposure subscription: %s", subscription_id.c_str());

  response_body = updated_subscription;
  http_code     = http_status_code::OK;
  nef_audit::log("UPDATE", "EE", "", subscription_id, http_code);
  rfl_response = j2g(response_body);
}

// Monitoring Event Exposure (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_create(
    const std::string& scs_as_id, const rfl::Generic& rfl_body,
    std::string& sub_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  Logger::nef_app().info(
      "Create monitoring event subscription for SCS/AS: %s", scs_as_id.c_str());

  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  // Validate required fields
  if (!body.contains("monitoringType") ||
      !body.contains("notificationDestination")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "monitoringType and notificationDestination are required");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
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
  sub->set_http_version(http_version);
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
      rfl_response = j2g(response_body);
      return;
    }
    sub->set_expire_time(expire_time);
  }

  add_subscription(sub_id, sub);
  ensure_af_profile(scs_as_id, sub_id);

  // Subscribe to AMF event-exposure southbound.
  std::string amf_sub_id;
  // TODO: should pass sub_id as well?
  if (!m_nef_client->subscribe_amf_event_exposure(
          body, amf_sub_id, http_version)) {
    Logger::nef_app().warn("Failed to subscribe to AMF event exposure");
    remove_subscription(sub_id);
    release_af_profile_subscription(scs_as_id, sub_id);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to create AMF monitoring subscription");
    rfl_response = j2g(response_body);
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
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_delete(
    const std::string& scs_as_id, const std::string& sub_id, int& http_code,
    uint8_t http_version) {
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
    m_nef_client->unsubscribe_amf_event_exposure(nf_sub_id, http_version);
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
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
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
    http_code    = http_status_code::OK;
    rfl_response = j2g(response_body);
    return;
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = j2g(response_body);
    return;
  }

  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    rfl_response = j2g(response_body);
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = http_status_code::OK;
  rfl_response  = j2g(response_body);
}

// Traffic Influence (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_create(
    const std::string& af_id, const rfl::Generic& rfl_body, std::string& ti_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  Logger::nef_app().info("Create TI subscription for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  // Validate required fields: at least one traffic filter must be present
  if (!body.contains("afAppId") && !body.contains("trafficFilters") &&
      !body.contains("ethTrafficFilters")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
        "required");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
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
  ti_sub->set_http_version(http_version);
  ti_sub->set_subscription_data(body);
  if (body.contains("notificationDestination") &&
      body["notificationDestination"].is_string()) {
    ti_sub->set_notification_uri(
        body["notificationDestination"].get<std::string>());
  }
  add_subscription(ti_id, ti_sub);

  std::string pcf_policy_id;
  uint32_t http_code_pcf = 0;
  const bool pcf_ok      = m_nef_client->create_pcf_policy_auth(
      body, pcf_policy_id, http_code_pcf, http_version);
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
    rfl_response = j2g(response_body);
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
    rfl_response = j2g(response_body);
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
  if (!m_nef_client->udr_put_influence_data(
          ti_id, body, http_code_udr, http_version)) {
    Logger::nef_app().warn(
        "UDR influence PUT failed for ti_id=%s (http=%u)", ti_id.c_str(),
        http_code_udr);
  }

  response_body              = body;
  response_body["afTransId"] = ti_id;
  http_code                  = http_status_code::CREATED;
  nef_audit::log("CREATE", "TI", af_id, ti_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_update(
    const std::string& af_id, const std::string& ti_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  if (!body.contains("afAppId") && !body.contains("trafficFilters") &&
      !body.contains("ethTrafficFilters")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
        "required");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
      return;
    }

    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      rfl_response = j2g(response_body);
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
    rfl_response = j2g(response_body);
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_policy_auth(
          pcf_policy_id, body, http_code_pcf, http_version)) {
    Logger::nef_app().warn(
        "PCF TI update failed for ti_id=%s policy_id=%s (http=%u)",
        ti_id.c_str(), pcf_policy_id.c_str(), http_code_pcf);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update policy authorization in PCF");
    rfl_response = j2g(response_body);
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      rfl_response = j2g(response_body);
      return;
    }
    session_it->second = body;
  }

  response_body = body;
  http_code     = http_status_code::OK;
  nef_audit::log("UPDATE", "TI", af_id, ti_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_delete(
    const std::string& af_id, const std::string& ti_id, int& http_code,
    uint8_t http_version) {
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
    if (!m_nef_client->delete_pcf_policy_auth(
            pcf_policy_id, http_code_pcf, http_version)) {
      Logger::nef_app().warn(
          "PCF TI delete failed for ti_id=%s policy_id=%s (http=%u)",
          ti_id.c_str(), pcf_policy_id.c_str(), http_code_pcf);
    }
  }

  uint32_t http_code_udr = 0;
  if (!m_nef_client->udr_delete_influence_data(
          ti_id, http_code_udr, http_version)) {
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
    const std::string& app_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  Logger::nef_app().info("PFD create for app: %s", app_id.c_str());

  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  if (!body.contains("pfdDatas")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: pfdDatas");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
      return;
    }
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, body, http_version)) {
    Logger::nef_app().warn("UDR PFD push failed for app: %s", app_id.c_str());
  }
  response_body = body;
  http_code     = http_status_code::CREATED;
  nef_audit::log("CREATE", "PFD", app_id, app_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_delete(
    const std::string& app_id, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }

  m_nef_client->udr_delete_pfd_data(app_id, http_version);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "PFD", app_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_get(
    const std::string& app_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  nlohmann::json result;
  uint32_t http_code_udr = 0;
  m_nef_client->udr_get_pfd_data(app_id, result, http_code_udr);

  if (http_code_udr == http_status_code::OK) {
    response_body = result;
    http_code     = http_status_code::OK;
    rfl_response  = j2g(response_body);
    return;
  }

  if (http_code_udr == http_status_code::NOT_FOUND) {
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD data not found");
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = j2g(response_body);
    return;
  }

  response_body = make_problem_detail(
      http_status_code::BAD_GATEWAY, "Bad Gateway",
      "UDR returned an unexpected response for PFD GET");
  http_code    = http_status_code::BAD_GATEWAY;
  rfl_response = j2g(response_body);
}

// BDT Policy (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_create(
    const std::string& af_id, const rfl::Generic& rfl_body, std::string& bdt_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  Logger::nef_app().info("BDT policy create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  if (!body.contains("bdtPolData")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: bdtPolData");
    rfl_response = j2g(response_body);
    return;
  }

  // Path-parameter and type validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty()) err = validate_object_field(body, "bdtPolData", false);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      rfl_response = j2g(response_body);
      return;
    }
  }

  generate_af_subscription_id(bdt_id);

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    m_bdt_sessions[bdt_id] = body;
    m_bdt_id2af_id[bdt_id] = af_id;
  }

  std::string pcf_bdt_id;
  uint32_t http_code_pcf = 0;
  const bool pcf_ok      = m_nef_client->create_pcf_bdt_policy(
      body, pcf_bdt_id, http_code_pcf, http_version);
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
    rfl_response = j2g(response_body);
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    m_bdt_id2pcf_policy_id[bdt_id] = pcf_bdt_id;
  }

  response_body             = body;
  response_body["bdtRefId"] = bdt_id;
  http_code                 = http_status_code::CREATED;
  nef_audit::log("CREATE", "BDT", af_id, bdt_id, http_code);
  rfl_response = j2g(response_body);
}

// BDT Policy (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_update(
    const std::string& af_id, const std::string& bdt_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  if (!body.contains("bdtPolData")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: bdtPolData");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
      return;
    }

    auto owner_it = m_bdt_id2af_id.find(bdt_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      rfl_response = j2g(response_body);
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
    rfl_response = j2g(response_body);
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_bdt_policy(
          pcf_bdt_id, body, http_code_pcf, http_version)) {
    Logger::nef_app().warn(
        "PCF BDT update failed for bdt_id=%s policy_id=%s (http=%u)",
        bdt_id.c_str(), pcf_bdt_id.c_str(), http_code_pcf);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update BDT policy in PCF");
    rfl_response = j2g(response_body);
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      rfl_response = j2g(response_body);
      return;
    }
    session_it->second = body;
  }

  response_body = body;
  http_code     = http_status_code::OK;
  nef_audit::log("UPDATE", "BDT", af_id, bdt_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_delete(
    const std::string& af_id, const std::string& bdt_id, int& http_code,
    uint8_t http_version) {
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
    if (!m_nef_client->delete_pcf_bdt_policy(
            pcf_bdt_id, http_code_pcf, http_version)) {
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
    const std::string& af_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  std::shared_lock lock(m_bdt_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [id, body] : m_bdt_sessions) {
    auto owner_it = m_bdt_id2af_id.find(id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      continue;
    }
    nlohmann::json entry = body;
    entry["bdtRefId"]    = id;
    response_body.push_back(entry);
  }
  http_code    = http_status_code::OK;
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_get(
    const std::string& af_id, const std::string& bdt_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  std::shared_lock lock(m_bdt_mutex);
  auto it = m_bdt_sessions.find(bdt_id);
  if (it == m_bdt_sessions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
    rfl_response = j2g(response_body);
    return;
  }

  auto owner_it = m_bdt_id2af_id.find(bdt_id);
  if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this resource");
    rfl_response = j2g(response_body);
    return;
  }

  response_body             = it->second;
  response_body["bdtRefId"] = bdt_id;
  http_code                 = http_status_code::OK;
  rfl_response              = j2g(response_body);
}

// QoS Provisioning (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_create(
    const std::string& af_id, const rfl::Generic& rfl_body,
    std::string& qos_sub_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  Logger::nef_app().info("QoS subscription create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  // Validate required fields
  if (!body.contains("notifUri") ||
      (!body.contains("flowInfo") && !body.contains("ethFlowInfo"))) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notifUri and at least one of flowInfo or ethFlowInfo are required");
    rfl_response = j2g(response_body);
    return;
  }

  // Path-parameter and field validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty()) err = validate_string_field(body, "notifUri", true, 2048);
    if (err.empty()) err = validate_string_field(body, "afAppId", false, 256);
    if (!err.empty()) {
      http_code     = http_status_code::UNPROCESSABLE_ENTITY;
      response_body = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      rfl_response = j2g(response_body);
      return;
    }
  }

  // SSRF protection: validate the callback URI before any storage or southbound
  // calls
  {
    const std::string uri_err =
        validate_callback_uri(body["notifUri"].get<std::string>());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
      rfl_response = j2g(response_body);
      return;
    }
  }

  generate_af_subscription_id(qos_sub_id);
  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(qos_sub_id);
  sub->set_scs_as_id(af_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING);
  sub->set_target_nf_type(nf_type_t::NF_TYPE_SMF);
  sub->set_http_version(http_version);
  sub->set_subscription_data(body);

  if (body.contains("requestExpiry") && body["requestExpiry"].is_string()) {
    std::chrono::system_clock::time_point expire_time;
    if (parse_monitor_expire_time(
            body["requestExpiry"].get<std::string>(), expire_time)) {
      sub->set_expire_time(expire_time);
      Logger::nef_app().debug(
          "F1.2: QoS subscription '%s' expiry set from requestExpiry",
          qos_sub_id.c_str());
    }
  }

  add_subscription(qos_sub_id, sub);
  ensure_af_profile(af_id, qos_sub_id);

  std::string smf_sub_id;
  const bool smf_ok = m_nef_client->subscribe_smf_event_exposure(
      body, smf_sub_id, http_version);
  if (!smf_ok || smf_sub_id.empty()) {
    remove_subscription(qos_sub_id);
    release_af_profile_subscription(af_id, qos_sub_id);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to create SMF event exposure subscription");
    rfl_response = j2g(response_body);
    return;
  }

  sub->set_nf_subscription_id(smf_sub_id);

  if (!smf_sub_id.empty()) {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[smf_sub_id] = qos_sub_id;
  }
  if (body.contains("notifUri")) {
    sub->set_notification_uri(body["notifUri"].get<std::string>());
  }

  response_body          = body;
  response_body["subId"] = qos_sub_id;
  http_code              = http_status_code::CREATED;
  nef_audit::log("CREATE", "QOS", af_id, qos_sub_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_delete(
    const std::string& af_id, const std::string& qos_sub_id, int& http_code,
    uint8_t http_version) {
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
    m_nef_client->unsubscribe_smf_event_exposure(nf_sub_id, http_version);
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(nf_sub_id);
  }

  remove_subscription(qos_sub_id);
  release_af_profile_subscription(af_id, qos_sub_id);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "QOS", af_id, qos_sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_get(
    const std::string& af_id, const std::string& qos_sub_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  auto sub = find_subscription(qos_sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "QoS subscription not found");
    rfl_response = j2g(response_body);
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    rfl_response = j2g(response_body);
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = http_status_code::OK;
  rfl_response  = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_list(
    const std::string& af_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
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
  http_code    = http_status_code::OK;
  rfl_response = j2g(response_body);
}

// Analytics Subscription (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_create(
    const std::string& af_id, const rfl::Generic& rfl_body,
    std::string& analytics_sub_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  Logger::nef_app().info(
      "Analytics subscription create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  if (!body.contains("analyEventsSubs") || !body.contains("notifUri") ||
      !body.contains("notifId")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "analyEventsSubs, notifUri, and notifId are required");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
      return;
    }
  }

  generate_af_subscription_id(analytics_sub_id);
  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(analytics_sub_id);
  sub->set_scs_as_id(af_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_ANALYTICS);
  sub->set_http_version(http_version);
  sub->set_subscription_data(body);

  add_subscription(analytics_sub_id, sub);
  ensure_af_profile(af_id, analytics_sub_id);
  response_body          = body;
  response_body["subId"] = analytics_sub_id;
  http_code              = http_status_code::CREATED;
  nef_audit::log("CREATE", "ANA", af_id, analytics_sub_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_delete(
    const std::string& af_id, const std::string& analytics_sub_id,
    int& http_code, uint8_t http_version) {
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
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }

  auto sub = find_subscription(analytics_sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Analytics subscription not found");
    rfl_response = j2g(response_body);
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    rfl_response = j2g(response_body);
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = http_status_code::OK;
  rfl_response  = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_list(
    const std::string& af_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
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
  http_code    = http_status_code::OK;
  rfl_response = j2g(response_body);
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
        if (!sub.contains("eventsRepInfo")) continue;
        const auto& rep = sub["eventsRepInfo"];
        if (!rep.contains("monDur") || !rep["monDur"].is_string()) continue;
        std::chrono::system_clock::time_point expire_tp;
        if (!parse_monitor_expire_time(
                rep["monDur"].get<std::string>(), expire_tp))
          continue;
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
    const auto http_version     = sub->get_http_version();
    const auto svc_type         = sub->get_service_type();

    // Service-specific southbound cleanup before removing local state.
    // TI subscriptions store the PCF policy ID in nf_sub_id and must call
    // delete_pcf_policy_auth rather than the NF event-exposure unsubscribe
    // paths.
    if (svc_type == nef_service_type_t::NEF_SERVICE_TYPE_TRAFFIC_INFLUENCE) {
      if (!nf_sub_id.empty()) {
        uint32_t http_code_pcf = 0;
        if (!m_nef_client->delete_pcf_policy_auth(
                nf_sub_id, http_code_pcf, http_version)) {
          Logger::nef_app().warn(
              "F1.3: PCF TI expiry delete failed for sub=%s (http=%u)",
              sub_id.c_str(), http_code_pcf);
        }
        uint32_t http_code_udr = 0;
        if (!m_nef_client->udr_delete_influence_data(
                sub_id, http_code_udr, http_version)) {
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
          m_nef_client->unsubscribe_amf_event_exposure(nf_sub_id, http_version);
        } else if (nf_type == nf_type_t::NF_TYPE_SMF) {
          m_nef_client->unsubscribe_smf_event_exposure(nf_sub_id, http_version);
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
    const rfl::Generic& patch_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
      return;
    }
    auto owner_it = m_ti_id2af_id.find(app_session_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      rfl_response = j2g(response_body);
      return;
    }
    auto pcf_it = m_ti_id2pcf_policy_id.find(app_session_id);
    if (pcf_it != m_ti_id2pcf_policy_id.end()) {
      pcf_policy_id = pcf_it->second;
    }
    auto r_base = rfl::json::read<rfl::Generic>(session_it->second.dump());
    if (!r_base) {
      http_code     = http_status_code::INTERNAL_SERVER_ERROR;
      response_body = make_problem_detail(
          http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
          "Failed to parse stored TI session");
      rfl_response = j2g(response_body);
      return;
    }
    const rfl::Generic r_result = nef_merge_patch(r_base.value(), patch_body);
    patched_copy = nlohmann::json::parse(rfl::json::write(r_result));
  }

  if (pcf_policy_id.empty()) {
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF policy identifier for TI session");
    rfl_response = j2g(response_body);
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_policy_auth(
          pcf_policy_id, patched_copy, http_code_pcf, http_version)) {
    Logger::nef_app().warn(
        "PCF TI patch failed for ti_id=%s (http=%u)", app_session_id.c_str(),
        http_code_pcf);
    http_code     = http_status_code::BAD_GATEWAY;
    response_body = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update policy authorization in PCF");
    rfl_response = j2g(response_body);
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(app_session_id);
    if (session_it == m_ti_sessions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      rfl_response = j2g(response_body);
      return;
    }
    session_it->second = patched_copy;
  }

  response_body              = patched_copy;
  response_body["afTransId"] = app_session_id;
  http_code                  = http_status_code::OK;
  rfl_response               = j2g(response_body);
  nef_audit::log("PATCH", "TI", af_id, app_session_id, http_code);
}

// QoS PATCH handler (TS 29.122)
//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_patch(
    const std::string& scs_as_id, const std::string& sub_id,
    const rfl::Generic& patch_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "QoS subscription not found");
    rfl_response = j2g(response_body);
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    rfl_response = j2g(response_body);
    return;
  }
  auto r_base =
      rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
  if (!r_base) {
    http_code     = http_status_code::INTERNAL_SERVER_ERROR;
    response_body = make_problem_detail(
        http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
        "Failed to parse stored QoS subscription");
    rfl_response = j2g(response_body);
    return;
  }
  const rfl::Generic r_result = nef_merge_patch(r_base.value(), patch_body);
  nlohmann::json patched = nlohmann::json::parse(rfl::json::write(r_result));
  // SSRF protection: validate callback URI in the patched result if present
  if (patched.contains("notifUri") && patched["notifUri"].is_string()) {
    const std::string uri_err =
        validate_callback_uri(patched["notifUri"].get<std::string>());
    if (!uri_err.empty()) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
      rfl_response = j2g(response_body);
      return;
    }
  }
  sub->set_subscription_data(patched);
  if (patched.contains("notifUri")) {
    sub->set_notification_uri(patched["notifUri"].get<std::string>());
  }
  response_body          = patched;
  response_body["subId"] = sub_id;
  http_code              = http_status_code::OK;
  rfl_response           = j2g(response_body);
  nef_audit::log("PATCH", "QOS", scs_as_id, sub_id, http_code);
}

// PFD transaction-level and app-level endpoints (TS 29.122)
//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_list(
    const std::string& scs_as_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }
  std::shared_lock lock(m_pfd_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [tid, body] : m_pfd_trans_sessions) {
    auto owner_it = m_pfd_trans2scs_id.find(tid);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id)
      continue;
    nlohmann::json entry = body;
    entry["transId"]     = tid;
    response_body.push_back(entry);
  }
  http_code    = http_status_code::OK;
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }
  if (!body.contains("pfdDatas")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: pfdDatas");
    rfl_response = j2g(response_body);
    return;
  }

  // Type validation (422 for semantic errors).
  if (!body["pfdDatas"].is_object()) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        "pfdDatas: must be an object");
    rfl_response = j2g(response_body);
    return;
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
  for (auto& [app_id, pfd_data] : body["pfdDatas"].items()) {
    if (!m_nef_client->udr_put_pfd_data(app_id, pfd_data, http_version)) {
      Logger::nef_app().error(
          "F1.10: UDR PFD write failed for app '%s' in trans '%s'; "
          "rolling back %zu committed app(s)",
          app_id.c_str(), trans_id.c_str(), pfd_rollback.committed_count());
      const int rb_failures = pfd_rollback.execute(
          [this, http_version](const std::string& rid) {
            return m_nef_client->udr_delete_pfd_data(rid, http_version);
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
      rfl_response = j2g(response_body);
      return;
    }
    pfd_rollback.mark_committed(app_id);
  }

  // All UDR writes succeeded — commit local state (deferred commit)
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    m_pfd_trans_sessions[trans_id] = body;
    m_pfd_trans2scs_id[trans_id]   = scs_as_id;
  }

  response_body            = body;
  response_body["transId"] = trans_id;
  http_code = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "PFD_TX", scs_as_id, trans_id,
      http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }
  nlohmann::json trans_body;
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
  if (trans_body.contains("pfdDatas") && trans_body["pfdDatas"].is_object()) {
    for (auto& [app_id, _] : trans_body["pfdDatas"].items()) {
      m_nef_client->udr_delete_pfd_data(app_id, http_version);
    }
  }
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "PFD_TX", scs_as_id, trans_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }
  std::shared_lock lock(m_pfd_mutex);
  auto it = m_pfd_trans_sessions.find(trans_id);
  if (it == m_pfd_trans_sessions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD transaction not found");
    rfl_response = j2g(response_body);
    return;
  }
  auto owner_it = m_pfd_trans2scs_id.find(trans_id);
  if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this resource");
    rfl_response = j2g(response_body);
    return;
  }
  const auto& pfd_datas =
      it->second.value("pfdDatas", nlohmann::json::object());
  if (!pfd_datas.contains(app_id)) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Application PFD not found in transaction");
    rfl_response = j2g(response_body);
    return;
  }
  response_body          = pfd_datas[app_id];
  response_body["appId"] = app_id;
  http_code              = http_status_code::OK;
  rfl_response           = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      rfl_response = j2g(response_body);
      return;
    }
    if (!it->second.contains("pfdDatas") ||
        !it->second["pfdDatas"].is_object()) {
      it->second["pfdDatas"] = nlohmann::json::object();
    }
    is_create                      = !it->second["pfdDatas"].contains(app_id);
    it->second["pfdDatas"][app_id] = body;
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, body, http_version)) {
    Logger::nef_app().warn(
        "UDR PFD app PUT failed for app: %s", app_id.c_str());
  }

  response_body          = body;
  response_body["appId"] = app_id;
  http_code = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "PFD_APP", scs_as_id, app_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const rfl::Generic& patch_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code     = http_status_code::FORBIDDEN;
      response_body = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      rfl_response = j2g(response_body);
      return;
    }
    const auto& pfd_datas =
        it->second.value("pfdDatas", nlohmann::json::object());
    if (!pfd_datas.contains(app_id)) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "Application PFD not found in transaction");
      rfl_response = j2g(response_body);
      return;
    }
    auto r_app =
        rfl::json::read<rfl::Generic>(it->second["pfdDatas"][app_id].dump());
    if (!r_app) {
      http_code     = http_status_code::INTERNAL_SERVER_ERROR;
      response_body = make_problem_detail(
          http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
          "Failed to parse stored PFD app entry");
      rfl_response = j2g(response_body);
      return;
    }
    const rfl::Generic r_result = nef_merge_patch(r_app.value(), patch_body);
    patched = nlohmann::json::parse(rfl::json::write(r_result));
    it->second["pfdDatas"][app_id] = patched;
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, patched, http_version)) {
    Logger::nef_app().warn(
        "UDR PFD app PATCH failed for app: %s", app_id.c_str());
  }

  response_body          = patched;
  response_body["appId"] = app_id;
  http_code              = http_status_code::OK;
  rfl_response           = j2g(response_body);
  nef_audit::log("PATCH", "PFD_APP", scs_as_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, int& http_code, uint8_t http_version) {
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
    if (!it->second.contains("pfdDatas") ||
        !it->second["pfdDatas"].contains(app_id)) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    it->second["pfdDatas"].erase(app_id);
  }
  m_nef_client->udr_delete_pfd_data(app_id, http_version);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "PFD_APP", scs_as_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_list_transactions(
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [transaction_id, transaction] : m_nnef_pfd_transactions) {
    response_body.push_back(transaction);
  }
  http_code    = http_status_code::OK;
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_put_transaction(
    const std::string& transaction_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }
  nlohmann::json applications;
  std::string error_detail;
  if (!extract_nnef_pfd_transaction_apps(body, applications, error_detail)) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    rfl_response = j2g(response_body);
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
    if (!m_nef_client->udr_put_pfd_data(app_id, app_body, http_version)) {
      Logger::nef_app().error(
          "UDR PFD write failed for Nnef app '%s' in trans '%s'; "
          "rolling back %zu committed app(s)",
          app_id.c_str(), transaction_id.c_str(),
          pfd_rollback.committed_count());
      const int rb_failures = pfd_rollback.execute(
          [this, http_version](const std::string& rid) {
            return m_nef_client->udr_delete_pfd_data(rid, http_version);
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
      rfl_response = j2g(response_body);
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
      if (!m_nef_client->udr_delete_pfd_data(app_id, http_version)) {
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
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_get_transaction(
    const std::string& transaction_id, rfl::Generic& rfl_response,
    int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto it = m_nnef_pfd_transactions.find(transaction_id);
  if (it == m_nnef_pfd_transactions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD transaction not found");
    rfl_response = j2g(response_body);
    return;
  }
  response_body = it->second;
  http_code     = http_status_code::OK;
  rfl_response  = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_delete_transaction(
    const std::string& transaction_id, int& http_code, uint8_t http_version) {
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
      if (!m_nef_client->udr_delete_pfd_data(app_id, http_version)) {
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
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto transaction_it = m_nnef_pfd_transactions.find(transaction_id);
  if (transaction_it == m_nnef_pfd_transactions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD transaction not found");
    rfl_response = j2g(response_body);
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
    rfl_response = j2g(response_body);
    return;
  }

  response_body = transaction["applications"][app_id];
  http_code     = http_status_code::OK;
  rfl_response  = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_put_app(
    const std::string& transaction_id, const std::string& app_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }
  std::string error_detail;
  nlohmann::json normalized_app;
  if (!normalize_nnef_pfd_app_data(
          app_id, body, normalized_app, error_detail)) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    rfl_response = j2g(response_body);
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

  if (!m_nef_client->udr_put_pfd_data(app_id, normalized_app, http_version)) {
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
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_delete_app(
    const std::string& transaction_id, const std::string& app_id,
    int& http_code, uint8_t http_version) {
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

  if (!m_nef_client->udr_delete_pfd_data(app_id, http_version)) {
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
    const std::vector<std::string>& app_ids_filter, rfl::Generic& rfl_response,
    int& http_code, uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
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
  http_code    = http_status_code::OK;
  rfl_response = j2g(response_body);
}

// Nnef_PFDmanagement — POST /applications/partial-pull
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_partial_pull(
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
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
  http_code    = http_status_code::OK;
  rfl_response = j2g(response_body);
}

// Nnef_PFDmanagement — subscription CRUD
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_create(
    const rfl::Generic& rfl_body, std::string& sub_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }
  // Validate required fields
  if (!body.contains("notifUri") || !body["notifUri"].is_string() ||
      body["notifUri"].get<std::string>().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri is required");
    rfl_response = j2g(response_body);
    return;
  }
  const std::string notif_uri = body["notifUri"].get<std::string>();
  const std::string uri_err   = validate_callback_uri(notif_uri);
  if (!uri_err.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
    rfl_response = j2g(response_body);
    return;
  }

  generate_af_subscription_id(sub_id);
  nlohmann::json stored = body;
  stored["subId"]       = sub_id;

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_subscriptions_mutex);
    m_nnef_pfd_subscriptions[sub_id] = stored;
  }
  response_body = stored;
  http_code     = http_status_code::CREATED;
  nef_audit::log("CREATE", "NNEF_PFD_SUB", "", sub_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_get(
    const std::string& sub_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }
  std::shared_lock lock(m_nnef_pfd_subscriptions_mutex);
  auto it = m_nnef_pfd_subscriptions.find(sub_id);
  if (it == m_nnef_pfd_subscriptions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD subscription not found");
    rfl_response = j2g(response_body);
    return;
  }
  response_body = it->second;
  http_code     = http_status_code::OK;
  rfl_response  = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_put(
    const std::string& sub_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    rfl_response = j2g(response_body);
    return;
  }
  // Full replace semantics (PUT)
  if (!body.contains("notifUri") || !body["notifUri"].is_string() ||
      body["notifUri"].get<std::string>().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri is required");
    rfl_response = j2g(response_body);
    return;
  }
  const std::string uri_err =
      validate_callback_uri(body["notifUri"].get<std::string>());
  if (!uri_err.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
      return;
    }
    // Full replace: discard old data, write new body entirely
    nlohmann::json stored = body;
    stored["subId"]       = sub_id;
    it->second            = stored;
    response_body         = stored;
  }
  http_code = http_status_code::OK;
  nef_audit::log("UPDATE", "NNEF_PFD_SUB", "", sub_id, http_code);
  rfl_response = j2g(response_body);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_delete(
    const std::string& sub_id, int& http_code, uint8_t http_version) {
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
      if (!sub.contains("notifUri") || !sub["notifUri"].is_string()) continue;
      // Use shared helper (checks "applicationIds" key — canonical TS 29.551
      // name)
      if (!nnef_pfd_subscription_matches(sub, app_id)) continue;
      targets.emplace_back(sid, sub["notifUri"].get<std::string>());
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
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body = g2j(rfl_body);
  nlohmann::json response_body;
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    rfl_response = j2g(response_body);
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Analytics subscription not found");
    rfl_response = j2g(response_body);
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    rfl_response = j2g(response_body);
    return;
  }
  if (!body.contains("analyEventsSubs") || !body.contains("notifUri") ||
      !body.contains("notifId")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "analyEventsSubs, notifUri, and notifId are required");
    rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
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
      rfl_response = j2g(response_body);
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
  rfl_response = j2g(response_body);
}
