/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
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

#include "logger.hpp"
#include "nef_client.hpp"
#include "nef_config.hpp"
#include "nef_config_types.hpp"
#include "nef_jwt.hpp"
#include "nef_notification_mapper.hpp"

#include <algorithm>

using namespace oai::nef::app;
using namespace boost::placeholders;

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

// F2.7: Analytics /fetch endpoint
void nef_app::handle_analytics_fetch(
    const std::string& scs_as_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }
  if (!body.contains("analyEventsSubs")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request", "Missing analyEventsSubs in request body");
    return;
  }
  Logger::nef_app().info(
      "Analytics fetch requested by %s: %s", scs_as_id.c_str(),
      body.dump().c_str());
  // Not implemented: just return stub
  http_code     = 501;
  response_body = make_problem_detail(
      501, "Not Implemented",
      "Analytics fetch is not implemented yet, but endpoint is available");
}
// F2.6: BDT PATCH
void nef_app::handle_bdt_policy_patch(
    const std::string& af_id, const std::string& bdt_policy_id,
    const nlohmann::json& patch_body, nlohmann::json& response_body,
    int& http_code, uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }
  std::string pcf_bdt_id;
  nlohmann::json patched_copy;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "BDT policy not found");
      return;
    }
    auto owner_it = m_bdt_id2af_id.find(bdt_policy_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code     = 403;
      response_body = make_problem_detail(
          403, "Forbidden", "AF is not allowed to access this resource");
      return;
    }
    auto pcf_it = m_bdt_id2pcf_policy_id.find(bdt_policy_id);
    if (pcf_it != m_bdt_id2pcf_policy_id.end()) {
      pcf_bdt_id = pcf_it->second;
    }
    patched_copy = session_it->second;
    patched_copy.merge_patch(patch_body);
  }
  if (pcf_bdt_id.empty()) {
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Missing PCF BDT policy identifier");
    return;
  }
  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_bdt_policy(
          pcf_bdt_id, patched_copy, http_code_pcf, http_version)) {
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Failed to update BDT policy in PCF");
    return;
  }
  {
    std::unique_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "BDT policy not found");
      return;
    }
    session_it->second = patched_copy;
  }
  response_body             = patched_copy;
  response_body["bdtRefId"] = bdt_policy_id;
  http_code                 = 200;
}
// F2.3: QoS UPDATE (PUT)
void nef_app::handle_qos_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code = 404;
    response_body =
        make_problem_detail(404, "Not Found", "QoS subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this subscription");
    return;
  }
  sub->set_subscription_data(body);
  if (body.contains("notifUri")) {
    sub->set_notification_uri(body["notifUri"].get<std::string>());
  }
  // Optionally: re-subscribe to SMF if needed (not implemented here)
  response_body          = sub->get_subscription_data();
  response_body["subId"] = sub_id;
  http_code              = 200;
}
// F2.2: Monitoring Event UPDATE (PUT)
void nef_app::handle_monitoring_event_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code = 404;
    response_body =
        make_problem_detail(404, "Not Found", "Subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this subscription");
    return;
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
  http_code              = 200;
}
// F2.1: TI GET
void nef_app::handle_traffic_influence_get(
    const std::string& af_id, const std::string& app_session_id,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }
  std::shared_lock lock(m_ti_mutex);
  auto it = m_ti_sessions.find(app_session_id);
  if (it == m_ti_sessions.end()) {
    http_code = 404;
    response_body =
        make_problem_detail(404, "Not Found", "TI session not found");
    return;
  }
  auto owner_it = m_ti_id2af_id.find(app_session_id);
  if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this resource");
    return;
  }
  response_body              = it->second;
  response_body["afTransId"] = app_session_id;
  http_code                  = 200;
}

// F2.1: TI LIST
void nef_app::handle_traffic_influence_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
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
  http_code = 200;
}

// ── RFC 7807 Problem Detail helper
// ────────────────────────────────────────────
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

  return true;
}

static std::string build_nnef_event_exposure_subscription_path(
    const std::string& subscription_id) {
  return "/nnef-eventexposure/v1/subscriptions/" + subscription_id;
}

static void finalize_nnef_event_exposure_subscription(
    nlohmann::json& subscription, const std::string& subscription_id) {
  subscription["subscriptionId"] = subscription_id;
  subscription["self"] =
      build_nnef_event_exposure_subscription_path(subscription_id);
}

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

// ── Constructor / Destructor
// ──────────────────────────────────────────────────
nef_app::nef_app(const std::string& config_file, nef_event& ev)
    : m_event_sub(ev) {
  Logger::nef_app().startup("Starting NEF application...");

  generate_uuid();

  // S0.5: Warn if AF whitelist is empty (development mode)
  {
    auto nef_cfg = nef_config_inst->nef();
    if (nef_cfg->get_af_whitelist().empty()) {
      Logger::nef_app().warn(
          "SECURITY WARNING: af_whitelist is empty — ALL Application Functions "
          "have unrestricted access (development mode only)");
    }
  }

  m_nef_client = std::make_shared<nef_client>();

  subscribe_nf_notification();

  // Register to NRF
  if (m_nef_client->register_to_nrf()) {
    Logger::nef_app().info("NEF registered to NRF");
  }

  // Periodic NRF heartbeat every 50 s (50000 ms ticks)
  constexpr uint64_t HEARTBEAT_MS = 50000;
  uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  auto hb_conn = m_event_sub.subscribe_task_tick(
      [this](uint64_t /*t*/) { m_nef_client->send_heartbeat_to_nrf(); },
      HEARTBEAT_MS, now_ms + HEARTBEAT_MS);
  m_connections.push_back(hb_conn);

  constexpr uint64_t SUBSCRIPTION_EXPIRY_CHECK_MS = 1000;
  auto expiry_conn = m_event_sub.subscribe_task_tick(
      [this](uint64_t t) { handle_subscription_expiry_tick(t); },
      SUBSCRIPTION_EXPIRY_CHECK_MS, now_ms + SUBSCRIPTION_EXPIRY_CHECK_MS);
  m_connections.push_back(expiry_conn);

  Logger::nef_app().startup("NEF application started");
}

nef_app::~nef_app() {
  Logger::nef_app().debug("Destroying NEF application...");
  for (auto& c : m_connections) {
    if (c.connected()) c.disconnect();
  }
  m_nef_client->deregister_from_nrf();
}

// ── Utility
// ───────────────────────────────────────────────────────────────────
void nef_app::generate_uuid() {
  m_nef_instance_id =
      boost::uuids::to_string(boost::uuids::random_generator()());
  Logger::nef_app().info("NEF instance ID: %s", m_nef_instance_id.c_str());
}

void nef_app::generate_af_subscription_id(std::string& sub_id) {
  uint32_t id = m_sub_id_generator.get_uid();
  std::ostringstream oss;
  oss << std::hex << id;
  sub_id = oss.str();
}

// ── Authorization
// ─────────────────────────────────────────────────────────────
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
  } else if (!jwt_secret.empty()) {
    Logger::nef_app().warn(
        "Missing bearer token for AF %s while jwt_secret is configured",
        scs_as_id.c_str());
    return false;
  }

  // Empty whitelist = open-access (development / test mode)
  if (wl.empty()) {
    Logger::nef_app().debug(
        "AF whitelist is empty – allowing %s on %s (dev mode)",
        scs_as_id.c_str(), api_name.c_str());
    return true;
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

void nef_app::set_request_bearer_token(const std::string& bearer_token) const {
  g_request_bearer_token = bearer_token;
}

void nef_app::clear_request_bearer_token() const {
  g_request_bearer_token.clear();
}

// ── Subscription helpers
// ──────────────────────────────────────────────────────
bool nef_app::add_subscription(
    const std::string& sub_id, const std::shared_ptr<nef_subscription>& s) {
  std::unique_lock lock(m_af_subscriptions_mutex);
  m_af_sub_id2subscription[sub_id] = s;
  return true;
}

bool nef_app::remove_subscription(const std::string& sub_id) {
  std::unique_lock lock(m_af_subscriptions_mutex);
  auto it = m_af_sub_id2subscription.find(sub_id);
  if (it == m_af_sub_id2subscription.end()) return false;
  m_af_sub_id2subscription.erase(it);
  return true;
}

std::shared_ptr<nef_subscription> nef_app::find_subscription(
    const std::string& sub_id) const {
  std::shared_lock lock(m_af_subscriptions_mutex);
  auto it = m_af_sub_id2subscription.find(sub_id);
  if (it != m_af_sub_id2subscription.end()) return it->second;
  return nullptr;
}

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

// ── AF Profile helpers
// ────────────────────────────────────────────────────────
bool nef_app::add_af_profile(
    const std::string& af_id, const std::shared_ptr<nef_af_profile>& p) {
  std::unique_lock lock(m_af_id2profile_mutex);
  m_af_id2profile[af_id] = p;
  Logger::nef_app().debug("AF profile added: %s", af_id.c_str());
  return true;
}

bool nef_app::remove_af_profile(const std::string& af_id) {
  std::unique_lock lock(m_af_id2profile_mutex);
  auto it = m_af_id2profile.find(af_id);
  if (it == m_af_id2profile.end()) return false;
  m_af_id2profile.erase(it);
  Logger::nef_app().debug("AF profile removed: %s", af_id.c_str());
  return true;
}

std::shared_ptr<nef_af_profile> nef_app::find_af_profile(
    const std::string& af_id) const {
  std::shared_lock lock(m_af_id2profile_mutex);
  auto it = m_af_id2profile.find(af_id);
  if (it != m_af_id2profile.end()) return it->second;
  return nullptr;
}

bool nef_app::is_af_registered(const std::string& af_id) const {
  std::shared_lock lock(m_af_id2profile_mutex);
  return m_af_id2profile.count(af_id) > 0;
}

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

// ── Event subscriptions
// ───────────────────────────────────────────────────────
void nef_app::subscribe_nf_notification() {
  auto conn = m_event_sub.subscribe_nf_notification(
      boost::bind(&nef_app::handle_nf_notification_event, this, _1, _2));
  m_connections.push_back(conn);
}

void nef_app::handle_nf_notification_event(
    const std::string& nf_sub_id, const nlohmann::json& notif) {
  handle_nf_notification(nf_sub_id, notif);
}

// ── Inbound notification from 5GC NF ─────────────────────────────────────────
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
    nlohmann::json t8_payload;
    bool mapped = false;
    auto svc    = sub->get_service_type();
    if (svc == nef_service_type_t::NEF_SERVICE_TYPE_MONITORING_EVENT) {
      mapped = nef_notification_mapper::amf_to_monitoring_notification(
          notif_payload, t8_payload, af_sub_id);
    } else if (svc == nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING) {
      mapped = nef_notification_mapper::smf_to_qos_notification(
          notif_payload, t8_payload, af_sub_id);
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

    const uint8_t http_ver = sub->get_http_version();
    auto nef_client        = m_nef_client;
    std::thread([nef_client, af_uri, t8_payload, http_ver]() {
      if (!nef_client->forward_notification_to_af(
              af_uri, t8_payload, http_ver)) {
        Logger::nef_app().warn(
            "Failed forwarding notification to AF endpoint: %s",
            af_uri.c_str());
      }
    }).detach();
  }
}

// ── Nnef_EventExposure (TS 29.591) ──────────────────────────────────────────
void nef_app::handle_nnef_event_exposure_subscribe(
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  (void) http_version;

  std::string error_detail;
  if (!validate_nnef_event_exposure_subscription(body, error_detail)) {
    http_code     = 400;
    response_body = make_problem_detail(400, "Bad Request", error_detail);
    return;
  }

  std::string subscription_id;
  generate_af_subscription_id(subscription_id);

  nlohmann::json stored_subscription = body;
  finalize_nnef_event_exposure_subscription(
      stored_subscription, subscription_id);

  {
    std::unique_lock lock(m_nnef_event_subscriptions_mutex);
    m_nnef_event_subscriptions[subscription_id] = stored_subscription;
  }

  Logger::nef_app().info(
      "Created Nnef_EventExposure subscription: %s", subscription_id.c_str());

  response_body = stored_subscription;
  http_code     = 201;
}

void nef_app::handle_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id, int& http_code, uint8_t http_version) {
  (void) http_version;

  std::unique_lock lock(m_nnef_event_subscriptions_mutex);
  auto it = m_nnef_event_subscriptions.find(subscription_id);
  if (it == m_nnef_event_subscriptions.end()) {
    http_code = 404;
    return;
  }

  m_nnef_event_subscriptions.erase(it);
  Logger::nef_app().info(
      "Deleted Nnef_EventExposure subscription: %s", subscription_id.c_str());
  http_code = 204;
}

void nef_app::handle_nnef_event_exposure_get(
    const std::string& subscription_id, nlohmann::json& response_body,
    int& http_code, uint8_t http_version) {
  (void) http_version;

  std::shared_lock lock(m_nnef_event_subscriptions_mutex);
  auto it = m_nnef_event_subscriptions.find(subscription_id);
  if (it == m_nnef_event_subscriptions.end()) {
    http_code     = 404;
    response_body = make_problem_detail(
        404, "Not Found", "Nnef_EventExposure subscription not found");
    return;
  }

  response_body = it->second;
  http_code     = 200;
}

void nef_app::handle_nnef_event_exposure_update(
    const std::string& subscription_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  (void) http_version;

  std::string error_detail;
  if (!validate_nnef_event_exposure_subscription(body, error_detail)) {
    http_code     = 400;
    response_body = make_problem_detail(400, "Bad Request", error_detail);
    return;
  }

  nlohmann::json updated_subscription = body;
  finalize_nnef_event_exposure_subscription(
      updated_subscription, subscription_id);

  {
    std::unique_lock lock(m_nnef_event_subscriptions_mutex);
    auto it = m_nnef_event_subscriptions.find(subscription_id);
    if (it == m_nnef_event_subscriptions.end()) {
      http_code     = 404;
      response_body = make_problem_detail(
          404, "Not Found", "Nnef_EventExposure subscription not found");
      return;
    }

    it->second = updated_subscription;
  }

  Logger::nef_app().info(
      "Updated Nnef_EventExposure subscription: %s", subscription_id.c_str());

  response_body = updated_subscription;
  http_code     = 200;
}

// ── Monitoring Event Exposure
// ─────────────────────────────────────────────────
void nef_app::handle_monitoring_event_subscription_create(
    const std::string& scs_as_id, const nlohmann::json& body,
    std::string& sub_id, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  Logger::nef_app().info(
      "Create monitoring event subscription for SCS/AS: %s", scs_as_id.c_str());

  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  // Validate required fields
  if (!body.contains("monitoringType") ||
      !body.contains("notificationDestination")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request",
        "monitoringType and notificationDestination are required");
    return;
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
      http_code     = 400;
      response_body = make_problem_detail(
          400, "Bad Request", "Invalid monitorExpireTime format");
      return;
    }
    sub->set_expire_time(expire_time);
  }

  add_subscription(sub_id, sub);
  ensure_af_profile(scs_as_id, sub_id);

  // Subscribe to AMF event-exposure southbound.
  std::string amf_sub_id;
  if (!m_nef_client->subscribe_amf_event_exposure(
          body, amf_sub_id, http_version)) {
    Logger::nef_app().warn("Failed to subscribe to AMF event exposure");
    remove_subscription(sub_id);
    release_af_profile_subscription(scs_as_id, sub_id);
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Failed to create AMF monitoring subscription");
    return;
  }

  sub->set_nf_subscription_id(amf_sub_id);
  if (!amf_sub_id.empty()) {
    std::unique_lock lock(m_nf2af_mutex);
    m_nf2af_sub_id[amf_sub_id] = sub_id;
  }

  response_body          = body;
  response_body["subId"] = sub_id;
  http_code              = 201;
}

void nef_app::handle_monitoring_event_subscription_delete(
    const std::string& scs_as_id, const std::string& sub_id, int& http_code,
    uint8_t http_version) {
  Logger::nef_app().info(
      "Delete monitoring event subscription: %s", sub_id.c_str());

  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code = 403;
    return;
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code = 404;
    return;
  }

  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code = 403;
    return;
  }

  // Unsubscribe from AMF
  std::string nf_sub_id = sub->get_nf_subscription_id();
  if (!nf_sub_id.empty()) {
    m_nef_client->unsubscribe_amf_event_exposure(nf_sub_id, http_version);
    std::unique_lock lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(nf_sub_id);
  }

  remove_subscription(sub_id);
  release_af_profile_subscription(scs_as_id, sub_id);
  http_code = 204;
}

void nef_app::handle_monitoring_event_subscription_get(
    const std::string& scs_as_id, const std::string& sub_id,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
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
    http_code = 200;
    return;
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code = 404;
    return;
  }

  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this subscription");
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = 200;
}

// ── Traffic Influence
// ─────────────────────────────────────────────────────────
void nef_app::handle_traffic_influence_create(
    const std::string& af_id, const nlohmann::json& body, std::string& ti_id,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  Logger::nef_app().info("Create TI subscription for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  // Validate required fields: at least one traffic filter must be present
  if (!body.contains("afAppId") && !body.contains("trafficFilters") &&
      !body.contains("ethTrafficFilters")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request",
        "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
        "required");
    return;
  }

  generate_af_subscription_id(ti_id);

  {
    std::unique_lock lock(m_ti_mutex);
    m_ti_sessions[ti_id] = body;
    m_ti_id2af_id[ti_id] = af_id;
  }

  std::string pcf_policy_id;
  uint32_t http_code_pcf = 0;
  const bool pcf_ok      = m_nef_client->create_pcf_policy_auth(
      body, pcf_policy_id, http_code_pcf, http_version);
  if (!pcf_ok || http_code_pcf < 200 || http_code_pcf >= 300) {
    Logger::nef_app().warn(
        "PCF TI create failed for ti_id=%s (http=%u), rolling back local "
        "session",
        ti_id.c_str(), http_code_pcf);
    {
      std::unique_lock lock(m_ti_mutex);
      m_ti_sessions.erase(ti_id);
      m_ti_id2af_id.erase(ti_id);
      m_ti_id2pcf_policy_id.erase(ti_id);
    }
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Failed to create policy authorization in PCF");
    return;
  }

  if (!pcf_policy_id.empty()) {
    std::unique_lock lock(m_ti_mutex);
    m_ti_id2pcf_policy_id[ti_id] = pcf_policy_id;
  }

  uint32_t http_code_udr = 0;
  if (!m_nef_client->udr_put_influence_data(
          ti_id, body, http_code_udr, http_version)) {
    Logger::nef_app().warn(
        "UDR influence PUT failed for ti_id=%s (http=%u)", ti_id.c_str(),
        http_code_udr);
  }

  response_body              = body;
  response_body["afTransId"] = ti_id;
  http_code                  = 201;
}

void nef_app::handle_traffic_influence_update(
    const std::string& af_id, const std::string& ti_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  if (!body.contains("afAppId") && !body.contains("trafficFilters") &&
      !body.contains("ethTrafficFilters")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request",
        "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
        "required");
    return;
  }

  std::string pcf_policy_id;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "TI session not found");
      return;
    }

    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code     = 403;
      response_body = make_problem_detail(
          403, "Forbidden", "AF is not allowed to access this resource");
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
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Missing PCF policy identifier for TI session");
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_policy_auth(
          pcf_policy_id, body, http_code_pcf, http_version)) {
    Logger::nef_app().warn(
        "PCF TI update failed for ti_id=%s policy_id=%s (http=%u)",
        ti_id.c_str(), pcf_policy_id.c_str(), http_code_pcf);
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Failed to update policy authorization in PCF");
    return;
  }

  {
    std::unique_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "TI session not found");
      return;
    }
    session_it->second = body;
  }

  response_body = body;
  http_code     = 200;
}

void nef_app::handle_traffic_influence_delete(
    const std::string& af_id, const std::string& ti_id, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code = 403;
    return;
  }

  std::string pcf_policy_id;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      http_code = 404;
      return;
    }

    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code = 403;
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
    std::unique_lock lock(m_ti_mutex);
    m_ti_sessions.erase(ti_id);
    m_ti_id2af_id.erase(ti_id);
    m_ti_id2pcf_policy_id.erase(ti_id);
  }
  http_code = 204;
}

// ── PFD Management
// ────────────────────────────────────────────────────────────
void nef_app::handle_pfd_create(
    const std::string& app_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  Logger::nef_app().info("PFD create for app: %s", app_id.c_str());

  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  if (!body.contains("pfdDatas")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request", "Missing required field: pfdDatas");
    return;
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, body, http_version)) {
    Logger::nef_app().warn("UDR PFD push failed for app: %s", app_id.c_str());
  }
  response_body = body;
  http_code     = 201;
}

void nef_app::handle_pfd_delete(
    const std::string& app_id, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = 403;
    return;
  }

  m_nef_client->udr_delete_pfd_data(app_id, http_version);
  http_code = 204;
}

void nef_app::handle_pfd_get(
    const std::string& app_id, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  nlohmann::json result;
  uint32_t http_code_udr = 0;
  m_nef_client->udr_get_pfd_data(app_id, result, http_code_udr);

  if (http_code_udr == 200) {
    response_body = result;
    http_code     = 200;
    return;
  }

  if (http_code_udr == 404) {
    response_body = make_problem_detail(404, "Not Found", "PFD data not found");
    http_code     = 404;
    return;
  }

  response_body = make_problem_detail(
      502, "Bad Gateway", "UDR returned an unexpected response for PFD GET");
  http_code = 502;
}

// ── BDT Policy
// ────────────────────────────────────────────────────────────────
void nef_app::handle_bdt_policy_create(
    const std::string& af_id, const nlohmann::json& body, std::string& bdt_id,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  Logger::nef_app().info("BDT policy create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  if (!body.contains("bdtPolData")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request", "Missing required field: bdtPolData");
    return;
  }

  generate_af_subscription_id(bdt_id);

  {
    std::unique_lock lock(m_bdt_mutex);
    m_bdt_sessions[bdt_id] = body;
    m_bdt_id2af_id[bdt_id] = af_id;
  }

  std::string pcf_bdt_id;
  uint32_t http_code_pcf = 0;
  const bool pcf_ok      = m_nef_client->create_pcf_bdt_policy(
      body, pcf_bdt_id, http_code_pcf, http_version);
  if (!pcf_ok ||
      ((http_code_pcf < 200 || http_code_pcf >= 300) && http_code_pcf != 303)) {
    Logger::nef_app().warn(
        "PCF BDT create failed for bdt_id=%s (http=%u), rolling back local "
        "session",
        bdt_id.c_str(), http_code_pcf);
    {
      std::unique_lock lock(m_bdt_mutex);
      m_bdt_sessions.erase(bdt_id);
      m_bdt_id2af_id.erase(bdt_id);
      m_bdt_id2pcf_policy_id.erase(bdt_id);
    }
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Failed to create BDT policy in PCF");
    return;
  }

  {
    std::unique_lock lock(m_bdt_mutex);
    m_bdt_id2pcf_policy_id[bdt_id] = pcf_bdt_id;
  }

  response_body             = body;
  response_body["bdtRefId"] = bdt_id;
  http_code                 = 201;
}

void nef_app::handle_bdt_policy_update(
    const std::string& af_id, const std::string& bdt_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  if (!body.contains("bdtPolData")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request", "Missing required field: bdtPolData");
    return;
  }

  std::string pcf_bdt_id;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "BDT policy not found");
      return;
    }

    auto owner_it = m_bdt_id2af_id.find(bdt_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code     = 403;
      response_body = make_problem_detail(
          403, "Forbidden", "AF is not allowed to access this resource");
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
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Missing PCF BDT policy identifier");
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_bdt_policy(
          pcf_bdt_id, body, http_code_pcf, http_version)) {
    Logger::nef_app().warn(
        "PCF BDT update failed for bdt_id=%s policy_id=%s (http=%u)",
        bdt_id.c_str(), pcf_bdt_id.c_str(), http_code_pcf);
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Failed to update BDT policy in PCF");
    return;
  }

  {
    std::unique_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "BDT policy not found");
      return;
    }
    session_it->second = body;
  }

  response_body = body;
  http_code     = 200;
}

void nef_app::handle_bdt_policy_delete(
    const std::string& af_id, const std::string& bdt_id, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code = 403;
    return;
  }

  std::string pcf_bdt_id;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code = 404;
      return;
    }

    auto owner_it = m_bdt_id2af_id.find(bdt_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code = 403;
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
    std::unique_lock lock(m_bdt_mutex);
    m_bdt_sessions.erase(bdt_id);
    m_bdt_id2af_id.erase(bdt_id);
    m_bdt_id2pcf_policy_id.erase(bdt_id);
  }
  http_code = 204;
}

void nef_app::handle_bdt_policy_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
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
  http_code = 200;
}

void nef_app::handle_bdt_policy_get(
    const std::string& af_id, const std::string& bdt_id,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  std::shared_lock lock(m_bdt_mutex);
  auto it = m_bdt_sessions.find(bdt_id);
  if (it == m_bdt_sessions.end()) {
    http_code = 404;
    response_body =
        make_problem_detail(404, "Not Found", "BDT policy not found");
    return;
  }

  auto owner_it = m_bdt_id2af_id.find(bdt_id);
  if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this resource");
    return;
  }

  response_body             = it->second;
  response_body["bdtRefId"] = bdt_id;
  http_code                 = 200;
}

// ── QoS Provisioning
// ──────────────────────────────────────────────────────────
void nef_app::handle_qos_subscription_create(
    const std::string& af_id, const nlohmann::json& body,
    std::string& qos_sub_id, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  Logger::nef_app().info("QoS subscription create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  // Validate required fields
  if (!body.contains("notifUri") ||
      (!body.contains("flowInfo") && !body.contains("ethFlowInfo"))) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request",
        "notifUri and at least one of flowInfo or ethFlowInfo are required");
    return;
  }

  generate_af_subscription_id(qos_sub_id);
  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(qos_sub_id);
  sub->set_scs_as_id(af_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING);
  sub->set_target_nf_type(nf_type_t::NF_TYPE_SMF);
  sub->set_http_version(http_version);
  sub->set_subscription_data(body);

  add_subscription(qos_sub_id, sub);
  ensure_af_profile(af_id, qos_sub_id);

  std::string smf_sub_id;
  const bool smf_ok = m_nef_client->subscribe_smf_event_exposure(
      body, smf_sub_id, http_version);
  if (!smf_ok || smf_sub_id.empty()) {
    remove_subscription(qos_sub_id);
    release_af_profile_subscription(af_id, qos_sub_id);
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Failed to create SMF event exposure subscription");
    return;
  }

  sub->set_nf_subscription_id(smf_sub_id);

  if (!smf_sub_id.empty()) {
    std::unique_lock lock(m_nf2af_mutex);
    m_nf2af_sub_id[smf_sub_id] = qos_sub_id;
  }
  if (body.contains("notifUri")) {
    sub->set_notification_uri(body["notifUri"].get<std::string>());
  }

  response_body          = body;
  response_body["subId"] = qos_sub_id;
  http_code              = 201;
}

void nef_app::handle_qos_subscription_delete(
    const std::string& af_id, const std::string& qos_sub_id, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code = 403;
    return;
  }

  auto sub = find_subscription(qos_sub_id);
  if (!sub) {
    http_code = 404;
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code = 403;
    return;
  }

  std::string nf_sub_id = sub->get_nf_subscription_id();
  if (!nf_sub_id.empty()) {
    m_nef_client->unsubscribe_smf_event_exposure(nf_sub_id, http_version);
    std::unique_lock lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(nf_sub_id);
  }

  remove_subscription(qos_sub_id);
  release_af_profile_subscription(af_id, qos_sub_id);
  http_code = 204;
}

void nef_app::handle_qos_subscription_get(
    const std::string& af_id, const std::string& qos_sub_id,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  auto sub = find_subscription(qos_sub_id);
  if (!sub) {
    http_code = 404;
    response_body =
        make_problem_detail(404, "Not Found", "QoS subscription not found");
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this subscription");
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = 200;
}

void nef_app::handle_qos_subscription_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
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
  http_code = 200;
}

// ── Analytics
// ─────────────────────────────────────────────────────────────────
void nef_app::handle_analytics_subscription_create(
    const std::string& af_id, const nlohmann::json& body,
    std::string& analytics_sub_id, nlohmann::json& response_body,
    int& http_code, uint8_t http_version) {
  Logger::nef_app().info(
      "Analytics subscription create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  if (!body.contains("analyEventsSubs") || !body.contains("notifUri") ||
      !body.contains("notifId")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request",
        "analyEventsSubs, notifUri, and notifId are required");
    return;
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
  http_code              = 201;
}

void nef_app::handle_analytics_subscription_delete(
    const std::string& af_id, const std::string& analytics_sub_id,
    int& http_code, uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code = 403;
    return;
  }

  auto sub = find_subscription(analytics_sub_id);
  if (!sub) {
    http_code = 404;
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code = 403;
    return;
  }

  if (!remove_subscription(analytics_sub_id)) {
    http_code = 404;
    return;
  }
  release_af_profile_subscription(af_id, analytics_sub_id);
  http_code = 204;
}

void nef_app::handle_analytics_subscription_get(
    const std::string& af_id, const std::string& analytics_sub_id,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  auto sub = find_subscription(analytics_sub_id);
  if (!sub) {
    http_code     = 404;
    response_body = make_problem_detail(
        404, "Not Found", "Analytics subscription not found");
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this subscription");
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = 200;
}

void nef_app::handle_analytics_subscription_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
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
  http_code = 200;
}

void nef_app::handle_subscription_expiry_tick(uint64_t t) {
  (void) t;
  const auto now = std::chrono::system_clock::now();

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
    if (!nf_sub_id.empty()) {
      if (nf_type == nf_type_t::NF_TYPE_AMF) {
        m_nef_client->unsubscribe_amf_event_exposure(nf_sub_id, http_version);
      } else if (nf_type == nf_type_t::NF_TYPE_SMF) {
        m_nef_client->unsubscribe_smf_event_exposure(nf_sub_id, http_version);
      }

      std::unique_lock lock(m_nf2af_mutex);
      m_nf2af_sub_id.erase(nf_sub_id);
    }

    remove_subscription(sub_id);
    release_af_profile_subscription(sub->get_scs_as_id(), sub_id);
    Logger::nef_app().info(
        "Subscription %s expired - cleaning up", sub_id.c_str());
  }
}

// ── F2.4: TI PATCH
// ────────────────────────────────────────────────────────────
void nef_app::handle_traffic_influence_patch(
    const std::string& af_id, const std::string& app_session_id,
    const nlohmann::json& patch_body, nlohmann::json& response_body,
    int& http_code, uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  std::string pcf_policy_id;
  nlohmann::json patched_copy;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(app_session_id);
    if (session_it == m_ti_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "TI session not found");
      return;
    }
    auto owner_it = m_ti_id2af_id.find(app_session_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code     = 403;
      response_body = make_problem_detail(
          403, "Forbidden", "AF is not allowed to access this resource");
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
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Missing PCF policy identifier for TI session");
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_policy_auth(
          pcf_policy_id, patched_copy, http_code_pcf, http_version)) {
    Logger::nef_app().warn(
        "PCF TI patch failed for ti_id=%s (http=%u)", app_session_id.c_str(),
        http_code_pcf);
    http_code     = 502;
    response_body = make_problem_detail(
        502, "Bad Gateway", "Failed to update policy authorization in PCF");
    return;
  }

  {
    std::unique_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(app_session_id);
    if (session_it == m_ti_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "TI session not found");
      return;
    }
    session_it->second = patched_copy;
  }

  response_body              = patched_copy;
  response_body["afTransId"] = app_session_id;
  http_code                  = 200;
}

// ── F2.5: QoS PATCH
// ───────────────────────────────────────────────────────────
void nef_app::handle_qos_subscription_patch(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& patch_body, nlohmann::json& response_body,
    int& http_code, uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code = 404;
    response_body =
        make_problem_detail(404, "Not Found", "QoS subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this subscription");
    return;
  }
  nlohmann::json patched = sub->get_subscription_data();
  patched.merge_patch(patch_body);
  sub->set_subscription_data(patched);
  if (patched.contains("notifUri")) {
    sub->set_notification_uri(patched["notifUri"].get<std::string>());
  }
  response_body          = patched;
  response_body["subId"] = sub_id;
  http_code              = 200;
}

// ── F2.8: PFD transaction-level and app-level endpoints ──────────────────────

void nef_app::handle_pfd_transaction_list(
    const std::string& scs_as_id, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
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
  http_code = 200;
}

void nef_app::handle_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }
  if (!body.contains("pfdDatas")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request", "Missing required field: pfdDatas");
    return;
  }

  bool is_create = false;
  {
    std::unique_lock lock(m_pfd_mutex);
    is_create =
        (m_pfd_trans_sessions.find(trans_id) == m_pfd_trans_sessions.end());
    m_pfd_trans_sessions[trans_id] = body;
    m_pfd_trans2scs_id[trans_id]   = scs_as_id;
  }

  // Sync each application's PFD data to UDR
  if (body["pfdDatas"].is_object()) {
    for (auto& [app_id, pfd_data] : body["pfdDatas"].items()) {
      if (!m_nef_client->udr_put_pfd_data(app_id, pfd_data, http_version)) {
        Logger::nef_app().warn(
            "UDR PFD push failed for app: %s in trans: %s", app_id.c_str(),
            trans_id.c_str());
      }
    }
  }

  response_body            = body;
  response_body["transId"] = trans_id;
  http_code                = is_create ? 201 : 200;
}

void nef_app::handle_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = 403;
    return;
  }
  nlohmann::json trans_body;
  {
    std::unique_lock lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code = 404;
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code = 403;
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
  http_code = 204;
}

void nef_app::handle_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }
  std::shared_lock lock(m_pfd_mutex);
  auto it = m_pfd_trans_sessions.find(trans_id);
  if (it == m_pfd_trans_sessions.end()) {
    http_code = 404;
    response_body =
        make_problem_detail(404, "Not Found", "PFD transaction not found");
    return;
  }
  auto owner_it = m_pfd_trans2scs_id.find(trans_id);
  if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this resource");
    return;
  }
  const auto& pfd_datas =
      it->second.value("pfdDatas", nlohmann::json::object());
  if (!pfd_datas.contains(app_id)) {
    http_code     = 404;
    response_body = make_problem_detail(
        404, "Not Found", "Application PFD not found in transaction");
    return;
  }
  response_body          = pfd_datas[app_id];
  response_body["appId"] = app_id;
  http_code              = 200;
}

void nef_app::handle_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  bool is_create = false;
  {
    std::unique_lock lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "PFD transaction not found");
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code     = 403;
      response_body = make_problem_detail(
          403, "Forbidden", "AF is not allowed to access this resource");
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
  http_code              = is_create ? 201 : 200;
}

void nef_app::handle_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& patch_body,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }

  nlohmann::json patched;
  {
    std::unique_lock lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code = 404;
      response_body =
          make_problem_detail(404, "Not Found", "PFD transaction not found");
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code     = 403;
      response_body = make_problem_detail(
          403, "Forbidden", "AF is not allowed to access this resource");
      return;
    }
    const auto& pfd_datas =
        it->second.value("pfdDatas", nlohmann::json::object());
    if (!pfd_datas.contains(app_id)) {
      http_code     = 404;
      response_body = make_problem_detail(
          404, "Not Found", "Application PFD not found in transaction");
      return;
    }
    patched = it->second["pfdDatas"][app_id];
    patched.merge_patch(patch_body);
    it->second["pfdDatas"][app_id] = patched;
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, patched, http_version)) {
    Logger::nef_app().warn(
        "UDR PFD app PATCH failed for app: %s", app_id.c_str());
  }

  response_body          = patched;
  response_body["appId"] = app_id;
  http_code              = 200;
}

void nef_app::handle_pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = 403;
    return;
  }
  {
    std::unique_lock lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code = 404;
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code = 403;
      return;
    }
    if (!it->second.contains("pfdDatas") ||
        !it->second["pfdDatas"].contains(app_id)) {
      http_code = 404;
      return;
    }
    it->second["pfdDatas"].erase(app_id);
  }
  m_nef_client->udr_delete_pfd_data(app_id, http_version);
  http_code = 204;
}

void nef_app::handle_nnef_pfd_list_transactions(
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [transaction_id, transaction] : m_nnef_pfd_transactions) {
    response_body.push_back(transaction);
  }
  http_code = 200;
}

void nef_app::handle_nnef_pfd_put_transaction(
    const std::string& transaction_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  nlohmann::json applications;
  std::string error_detail;
  if (!extract_nnef_pfd_transaction_apps(body, applications, error_detail)) {
    http_code     = 400;
    response_body = make_problem_detail(400, "Bad Request", error_detail);
    return;
  }

  nlohmann::json transaction =
      make_nnef_pfd_transaction(transaction_id, body, applications);
  bool is_create = false;
  nlohmann::json previous_transaction;
  {
    std::unique_lock lock(m_nnef_pfd_transactions_mutex);
    auto it   = m_nnef_pfd_transactions.find(transaction_id);
    is_create = (it == m_nnef_pfd_transactions.end());
    if (!is_create) {
      previous_transaction = it->second;
    }
    m_nnef_pfd_transactions[transaction_id] = transaction;
  }

  for (const auto& [app_id, app_body] : applications.items()) {
    if (!m_nef_client->udr_put_pfd_data(app_id, app_body, http_version)) {
      Logger::nef_app().warn(
          "UDR PFD push failed for Nnef_PFDmanagement app: %s in transaction: "
          "%s",
          app_id.c_str(), transaction_id.c_str());
    }
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
  http_code     = is_create ? 201 : 200;
}

void nef_app::handle_nnef_pfd_get_transaction(
    const std::string& transaction_id, nlohmann::json& response_body,
    int& http_code, uint8_t http_version) {
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto it = m_nnef_pfd_transactions.find(transaction_id);
  if (it == m_nnef_pfd_transactions.end()) {
    http_code = 404;
    response_body =
        make_problem_detail(404, "Not Found", "PFD transaction not found");
    return;
  }
  response_body = it->second;
  http_code     = 200;
}

void nef_app::handle_nnef_pfd_delete_transaction(
    const std::string& transaction_id, int& http_code, uint8_t http_version) {
  nlohmann::json transaction;
  {
    std::unique_lock lock(m_nnef_pfd_transactions_mutex);
    auto it = m_nnef_pfd_transactions.find(transaction_id);
    if (it == m_nnef_pfd_transactions.end()) {
      http_code = 404;
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
  http_code = 204;
}

void nef_app::handle_nnef_pfd_get_app(
    const std::string& transaction_id, const std::string& app_id,
    nlohmann::json& response_body, int& http_code, uint8_t http_version) {
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto transaction_it = m_nnef_pfd_transactions.find(transaction_id);
  if (transaction_it == m_nnef_pfd_transactions.end()) {
    http_code = 404;
    response_body =
        make_problem_detail(404, "Not Found", "PFD transaction not found");
    return;
  }

  const auto& transaction = transaction_it->second;
  if (!transaction.contains("applications") ||
      !transaction["applications"].is_object() ||
      !transaction["applications"].contains(app_id)) {
    http_code     = 404;
    response_body = make_problem_detail(
        404, "Not Found", "Application PFD not found in transaction");
    return;
  }

  response_body = transaction["applications"][app_id];
  http_code     = 200;
}

void nef_app::handle_nnef_pfd_put_app(
    const std::string& transaction_id, const std::string& app_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  std::string error_detail;
  nlohmann::json normalized_app;
  if (!normalize_nnef_pfd_app_data(
          app_id, body, normalized_app, error_detail)) {
    http_code     = 400;
    response_body = make_problem_detail(400, "Bad Request", error_detail);
    return;
  }

  bool is_create = false;
  nlohmann::json transaction_snapshot;
  {
    std::unique_lock lock(m_nnef_pfd_transactions_mutex);
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
  http_code     = is_create ? 201 : 200;
}

void nef_app::handle_nnef_pfd_delete_app(
    const std::string& transaction_id, const std::string& app_id,
    int& http_code, uint8_t http_version) {
  {
    std::unique_lock lock(m_nnef_pfd_transactions_mutex);
    auto transaction_it = m_nnef_pfd_transactions.find(transaction_id);
    if (transaction_it == m_nnef_pfd_transactions.end()) {
      http_code = 404;
      return;
    }
    auto& transaction = transaction_it->second;
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object() ||
        !transaction["applications"].contains(app_id)) {
      http_code = 404;
      return;
    }
    transaction["applications"].erase(app_id);
  }

  if (!m_nef_client->udr_delete_pfd_data(app_id, http_version)) {
    Logger::nef_app().warn(
        "UDR PFD app DELETE failed for Nnef_PFDmanagement app: %s",
        app_id.c_str());
  }
  http_code = 204;
}

// ── F2.9: Analytics UPDATE (PUT)
// ──────────────────────────────────────────────
void nef_app::handle_analytics_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_ANALYTICS)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = 404;
    response_body = make_problem_detail(
        404, "Not Found", "Analytics subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = 403;
    response_body = make_problem_detail(
        403, "Forbidden", "AF is not allowed to access this subscription");
    return;
  }
  if (!body.contains("analyEventsSubs") || !body.contains("notifUri") ||
      !body.contains("notifId")) {
    http_code     = 400;
    response_body = make_problem_detail(
        400, "Bad Request",
        "analyEventsSubs, notifUri, and notifId are required");
    return;
  }
  sub->set_subscription_data(body);
  if (body.contains("notifUri")) {
    sub->set_notification_uri(body["notifUri"].get<std::string>());
  }
  response_body          = body;
  response_body["subId"] = sub_id;
  http_code              = 200;
}
