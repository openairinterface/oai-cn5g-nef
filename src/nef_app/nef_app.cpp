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

static rfl::Generic make_problem_detail(
    int status, const std::string& title, const std::string& detail,
    const std::string& instance = "");

static bool validate_nnef_event_exposure_subscription(
    const rfl::Generic& body, std::string& error_detail);

static std::string build_nnef_event_exposure_subscription_path(
    const std::string& subscription_id);

static void finalize_nnef_event_exposure_subscription(
    rfl::Generic& subscription, const std::string& subscription_id);

static bool parse_monitor_expire_time(
    const std::string& value,
    std::chrono::system_clock::time_point& expire_time);

static bool normalize_nnef_pfd_app_data(
    const std::string& app_id, const rfl::Generic& input, rfl::Generic& output,
    std::string& error_detail);

static bool extract_nnef_pfd_transaction_apps(
    const rfl::Generic& body, rfl::Generic& applications,
    std::string& error_detail);

static rfl::Generic make_nnef_pfd_transaction(
    const std::string& transaction_id, const rfl::Generic& body,
    const rfl::Generic& applications);

namespace {
thread_local std::string g_request_bearer_token;
}

// Helper: find a key in an rfl::Generic::Object.
// rfl::Object::find() is private; use std::find_if over the public range.
template<class Obj>
static inline auto rfl_obj_find(Obj& obj, std::string_view key) noexcept
    -> decltype(obj.begin()) {
  return std::find_if(
      obj.begin(), obj.end(), [key](const auto& p) { return p.first == key; });
}

// Helper: erase a key from an rfl::Generic::Object.
// rfl::Object has no erase(); rebuild without the target key.
static inline void rfl_obj_erase(
    rfl::Generic::Object& obj, std::string_view key) {
  rfl::Generic::Object tmp;
  for (auto& p : obj) {
    if (p.first != key) tmp.insert(std::move(p));
  }
  obj = std::move(tmp);
}

// Analytics /fetch endpoint
//------------------------------------------------------------------------------
void nef_app::handle_analytics_fetch(
    const std::string& scs_as_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_ANALYTICS)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  const auto* req_obj = std::get_if<rfl::Generic::Object>(&rfl_body.variant());
  if (!req_obj || rfl_obj_find(*req_obj, "analyEventsSubs") == req_obj->end()) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing analyEventsSubs in request body");
    return;
  }
  Logger::nef_app().info(
      "Analytics fetch requested by %s: %s", scs_as_id.c_str(),
      rfl::json::write(rfl_body).c_str());

  const rfl::Generic& requested_events = req_obj->at("analyEventsSubs");
  const auto* req_arr =
      std::get_if<std::vector<rfl::Generic>>(&requested_events.variant());

  std::vector<rfl::Generic> reports;
  {
    std::shared_lock lock(m_af_subscriptions_mutex);
    for (const auto& [sub_id, sub] : m_af_sub_id2subscription) {
      if (sub->get_service_type() !=
          nef_service_type_t::NEF_SERVICE_TYPE_ANALYTICS)
        continue;
      if (sub->get_scs_as_id() != scs_as_id) continue;
      const auto rfl_sub_r =
          rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
      if (!rfl_sub_r) continue;
      const rfl::Generic& rfl_sub_data = rfl_sub_r.value();
      const auto* sub_obj =
          std::get_if<rfl::Generic::Object>(&rfl_sub_data.variant());
      if (!sub_obj) continue;
      auto eas_it = rfl_obj_find(*sub_obj, "analyEventsSubs");
      if (eas_it == sub_obj->end()) continue;
      const auto* stored_arr =
          std::get_if<std::vector<rfl::Generic>>(&eas_it->second.variant());
      if (!stored_arr) continue;

      bool match = false;
      if (req_arr) {
        for (const auto& req_ev : *req_arr) {
          const auto* req_ev_obj =
              std::get_if<rfl::Generic::Object>(&req_ev.variant());
          if (!req_ev_obj) continue;
          auto req_ae_it = rfl_obj_find(*req_ev_obj, "analyEvent");
          if (req_ae_it == req_ev_obj->end()) continue;
          for (const auto& stored_ev : *stored_arr) {
            const auto* stored_ev_obj =
                std::get_if<rfl::Generic::Object>(&stored_ev.variant());
            if (!stored_ev_obj) continue;
            auto stored_ae_it = rfl_obj_find(*stored_ev_obj, "analyEvent");
            if (stored_ae_it != stored_ev_obj->end()) {
              // analyEvent is always a string enum — compare string values
              const auto* stored_s =
                  std::get_if<std::string>(&stored_ae_it->second.variant());
              const auto* req_s =
                  std::get_if<std::string>(&req_ae_it->second.variant());
              if (stored_s && req_s && *stored_s == *req_s) {
                match = true;
                break;
              }
            }
          }
          if (match) break;
        }
      }
      if (!match) continue;

      rfl::Generic::Object report_obj;
      report_obj["subId"]           = rfl::Generic(sub_id);
      report_obj["analyEventsSubs"] = eas_it->second;
      auto nuri_it                  = rfl_obj_find(*sub_obj, "notifUri");
      if (nuri_it != sub_obj->end()) {
        report_obj["notifUri"] = nuri_it->second;
      }
      reports.push_back(rfl::Generic(report_obj));
    }
  }

  rfl::Generic::Object resp_obj;
  resp_obj["analyEventsSubs"]     = requested_events;
  resp_obj["noNetworkSupportInd"] = rfl::Generic(reports.empty());
  if (!reports.empty()) resp_obj["analyReports"] = rfl::Generic(reports);
  http_code    = http_status_code::OK;
  rfl_response = rfl::Generic(resp_obj);
}

// BDT PATCH
//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_patch(
    const std::string& af_id, const std::string& bdt_policy_id,
    const rfl::Generic& patch_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  std::string pcf_bdt_id;
  rfl::Generic r_result;
  nlohmann::json patched_copy;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      return;
    }
    auto owner_it = m_bdt_id2af_id.find(bdt_policy_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code    = http_status_code::FORBIDDEN;
      rfl_response = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }
    auto pcf_it = m_bdt_id2pcf_policy_id.find(bdt_policy_id);
    if (pcf_it != m_bdt_id2pcf_policy_id.end()) {
      pcf_bdt_id = pcf_it->second;
    }
    // session_it->second is already rfl::Generic — no .dump() needed
    r_result     = nef_merge_patch(session_it->second, patch_body);
    patched_copy = nlohmann::json::parse(rfl::json::write(r_result));
  }
  if (pcf_bdt_id.empty()) {
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF BDT policy identifier");
    return;
  }
  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_bdt_policy(
          pcf_bdt_id, patched_copy, http_code_pcf, http_version)) {
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update BDT policy in PCF");
    return;
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_policy_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      return;
    }
    session_it->second = r_result;
  }
  rfl_response = r_result;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["bdtRefId"] = rfl::Generic(bdt_policy_id);
  }
  http_code = http_status_code::OK;
  nef_audit::log("PATCH", "BDT", af_id, bdt_policy_id, http_code);
}

// QoS UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "QoS subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }
  // Type and length validation (422 for semantic errors) — out-of-scope
  // boundary.
  {
    const nlohmann::json body_nj =
        nlohmann::json::parse(rfl::json::write(rfl_body));
    const std::string err =
        validate_string_field(body_nj, "notifUri", false, 2048);
    if (!err.empty()) {
      http_code    = http_status_code::UNPROCESSABLE_ENTITY;
      rfl_response = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }
  // SSRF protection: rfl::Generic-native field navigation.
  const auto* body_obj = std::get_if<rfl::Generic::Object>(&rfl_body.variant());
  if (body_obj) {
    auto it = rfl_obj_find(*body_obj, "notifUri");
    if (it != body_obj->end()) {
      if (const auto* s = std::get_if<std::string>(&it->second.variant())) {
        const std::string uri_err = validate_callback_uri(*s);
        if (!uri_err.empty()) {
          http_code    = http_status_code::BAD_REQUEST;
          rfl_response = make_problem_detail(
              http_status_code::BAD_REQUEST, "Bad Request",
              "notifUri: " + uri_err);
          return;
        }
      }
    }
  }
  // Update stored subscription — out-of-scope boundary.
  sub->set_subscription_data(nlohmann::json::parse(rfl::json::write(rfl_body)));
  if (body_obj) {
    auto it = rfl_obj_find(*body_obj, "notifUri");
    if (it != body_obj->end()) {
      if (const auto* s = std::get_if<std::string>(&it->second.variant())) {
        sub->set_notification_uri(*s);
      }
    }
  }
  // Optionally: re-subscribe to SMF if needed (not implemented here)
  {
    auto rfl_r =
        rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
    rfl_response = rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
  }
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["subId"] = rfl::Generic(sub_id);
  }
  http_code = http_status_code::OK;
  nef_audit::log("UPDATE", "QOS", scs_as_id, sub_id, http_code);
}

// Monitoring Event UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "Subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }
  // rfl::Generic-native field navigation for in-handler logic.
  const auto* body_obj = std::get_if<rfl::Generic::Object>(&rfl_body.variant());
  // SSRF protection: validate callback URI before updating stored state.
  if (body_obj) {
    auto it = rfl_obj_find(*body_obj, "notificationDestination");
    if (it != body_obj->end()) {
      if (const auto* s = std::get_if<std::string>(&it->second.variant())) {
        const std::string uri_err = validate_callback_uri(*s);
        if (!uri_err.empty()) {
          http_code    = http_status_code::BAD_REQUEST;
          rfl_response = make_problem_detail(
              http_status_code::BAD_REQUEST, "Bad Request",
              "notificationDestination: " + uri_err);
          return;
        }
      }
    }
  }
  // Update stored subscription — out-of-scope boundary.
  sub->set_subscription_data(nlohmann::json::parse(rfl::json::write(rfl_body)));
  if (body_obj) {
    auto it = rfl_obj_find(*body_obj, "notificationDestination");
    if (it != body_obj->end()) {
      if (const auto* s = std::get_if<std::string>(&it->second.variant())) {
        sub->set_notification_uri(*s);
      }
    }
  }
  if (body_obj) {
    auto it = rfl_obj_find(*body_obj, "monitorExpireTime");
    if (it != body_obj->end()) {
      if (const auto* s = std::get_if<std::string>(&it->second.variant())) {
        std::chrono::system_clock::time_point expire_time;
        if (parse_monitor_expire_time(*s, expire_time)) {
          sub->set_expire_time(expire_time);
        }
      }
    }
  }
  // Optionally: re-subscribe to AMF if needed (not implemented here)
  {
    auto rfl_r =
        rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
    rfl_response = rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
  }
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["subId"] = rfl::Generic(sub_id);
  }
  http_code = http_status_code::OK;
  nef_audit::log("UPDATE", "ME", scs_as_id, sub_id, http_code);
}

// TI GET
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_get(
    const std::string& af_id, const std::string& app_session_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  std::shared_lock lock(m_ti_mutex);
  auto it = m_ti_sessions.find(app_session_id);
  if (it == m_ti_sessions.end()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "TI session not found");
    return;
  }
  auto owner_it = m_ti_id2af_id.find(app_session_id);
  if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this resource");
    return;
  }
  rfl_response = it->second;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["afTransId"] = rfl::Generic(app_session_id);
  }
  http_code = http_status_code::OK;
}

// TI LIST
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_list(
    const std::string& af_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  std::shared_lock lock(m_ti_mutex);
  std::vector<rfl::Generic> result_arr;
  for (const auto& [id, session] : m_ti_sessions) {
    auto owner_it = m_ti_id2af_id.find(id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) continue;
    rfl::Generic entry = session;
    if (auto* e_obj = std::get_if<rfl::Generic::Object>(&entry.variant())) {
      (*e_obj)["afTransId"] = rfl::Generic(id);
    }
    result_arr.push_back(std::move(entry));
  }
  http_code    = http_status_code::OK;
  rfl_response = rfl::Generic(result_arr);
}

// RFC 7807 Problem Detail helper
static rfl::Generic make_problem_detail(
    int status, const std::string& title, const std::string& detail,
    const std::string& instance) {
  rfl::Generic::Object obj;
  obj["type"]   = rfl::Generic(std::string("about:blank"));
  obj["title"]  = rfl::Generic(title);
  obj["status"] = rfl::Generic(static_cast<double>(status));
  obj["detail"] = rfl::Generic(detail);
  if (!instance.empty()) obj["instance"] = rfl::Generic(instance);
  return rfl::Generic(obj);
}

static bool validate_nnef_event_exposure_subscription(
    const rfl::Generic& body, std::string& error_detail) {
  const auto* obj = std::get_if<rfl::Generic::Object>(&body.variant());
  if (!obj) {
    error_detail = "Request body must be a JSON object";
    return false;
  }

  // Check eventsSubs: required, non-empty array
  auto events_it = rfl_obj_find(*obj, "eventsSubs");
  if (events_it == obj->end()) {
    error_detail = "eventsSubs is required and must be a non-empty array";
    return false;
  }
  const auto* events_arr =
      std::get_if<std::vector<rfl::Generic>>(&events_it->second.variant());
  if (!events_arr || events_arr->empty()) {
    error_detail = "eventsSubs is required and must be a non-empty array";
    return false;
  }

  // TS 29.591 §5.4.2: each NefEventSubs item must have an "event" field.
  std::size_t idx = 0;
  for (const auto& item : *events_arr) {
    const auto* item_obj = std::get_if<rfl::Generic::Object>(&item.variant());
    if (!item_obj) {
      error_detail =
          "eventsSubs[" + std::to_string(idx) + "]: must be an object";
      return false;
    }
    auto ev_it = rfl_obj_find(*item_obj, "event");
    if (ev_it == item_obj->end()) {
      error_detail = "eventsSubs[" + std::to_string(idx) +
                     "].event: required non-empty string";
      return false;
    }
    const auto* ev_str = std::get_if<std::string>(&ev_it->second.variant());
    if (!ev_str || ev_str->empty()) {
      error_detail = "eventsSubs[" + std::to_string(idx) +
                     "].event: required non-empty string";
      return false;
    }
    ++idx;
  }

  // Check notifUri: required non-empty string
  auto notif_it = rfl_obj_find(*obj, "notifUri");
  if (notif_it == obj->end()) {
    error_detail = "notifUri is required and must be a non-empty string";
    return false;
  }
  const auto* notif_str = std::get_if<std::string>(&notif_it->second.variant());
  if (!notif_str || notif_str->empty()) {
    error_detail = "notifUri is required and must be a non-empty string";
    return false;
  }

  // Check notifId: required non-empty string
  auto id_it = rfl_obj_find(*obj, "notifId");
  if (id_it == obj->end()) {
    error_detail = "notifId is required and must be a non-empty string";
    return false;
  }
  const auto* id_str = std::get_if<std::string>(&id_it->second.variant());
  if (!id_str || id_str->empty()) {
    error_detail = "notifId is required and must be a non-empty string";
    return false;
  }

  error_detail = validate_callback_uri(*notif_str);
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
    rfl::Generic& subscription, const std::string& subscription_id) {
  auto* obj = std::get_if<rfl::Generic::Object>(&subscription.variant());
  if (!obj) return;
  // Use operator[] for writes: rfl_obj_find is for reads. operator[] either
  // updates an existing key or appends a new one — correct for both cases here.
  (*obj)["subscriptionId"] = rfl::Generic(subscription_id);
  (*obj)["self"]           = rfl::Generic(
      build_nnef_event_exposure_subscription_path(subscription_id));
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
    const std::string& app_id, const rfl::Generic& input, rfl::Generic& output,
    std::string& error_detail) {
  const auto* in_obj = std::get_if<rfl::Generic::Object>(&input.variant());
  if (!in_obj) {
    error_detail = "Application PFD must be a JSON object";
    return false;
  }

  // Copy input → output
  output    = input;
  auto* obj = std::get_if<rfl::Generic::Object>(&output.variant());
  // obj is valid since output is a copy of input which is an object

  // externalAppId
  auto ext_it = rfl_obj_find(*obj, "externalAppId");
  if (ext_it != obj->end()) {
    const auto* s = std::get_if<std::string>(&ext_it->second.variant());
    if (!s) {
      error_detail = "externalAppId must be a string";
      return false;
    }
    if (*s != app_id) {
      error_detail = "externalAppId must match the target appId";
      return false;
    }
  } else {
    (*obj)["externalAppId"] = rfl::Generic(app_id);
  }

  // pfds: required object
  auto pfds_it = rfl_obj_find(*obj, "pfds");
  if (pfds_it == obj->end()) {
    error_detail = "Missing required field: pfds";
    return false;
  }
  auto* pfds_obj =
      std::get_if<rfl::Generic::Object>(&pfds_it->second.variant());
  if (!pfds_obj) {
    error_detail = "Missing required field: pfds";
    return false;
  }

  for (auto& [pfd_id, pfd_content] : *pfds_obj) {
    auto* pfd_obj = std::get_if<rfl::Generic::Object>(&pfd_content.variant());
    if (!pfd_obj) {
      error_detail = "Each PFD entry must be a JSON object";
      return false;
    }

    auto pid_it = rfl_obj_find(*pfd_obj, "pfdId");
    if (pid_it != pfd_obj->end()) {
      const auto* pid_s = std::get_if<std::string>(&pid_it->second.variant());
      if (!pid_s) {
        error_detail = "pfdId must be a string";
        return false;
      }
      if (*pid_s != pfd_id) {
        error_detail = "pfdId must match its map key";
        return false;
      }
    } else {
      (*pfd_obj)["pfdId"] = rfl::Generic(pfd_id);
    }

    // flowDescriptions, urls, domainNames: must be arrays if present
    for (const auto& arr_field : {"flowDescriptions", "urls", "domainNames"}) {
      auto fit = rfl_obj_find(*pfd_obj, arr_field);
      if (fit != pfd_obj->end()) {
        if (!std::get_if<std::vector<rfl::Generic>>(&fit->second.variant())) {
          error_detail =
              std::string(arr_field) + " must be an array when present";
          return false;
        }
      }
    }
  }

  return true;
}

//------------------------------------------------------------------------------
static bool extract_nnef_pfd_transaction_apps(
    const rfl::Generic& body, rfl::Generic& applications,
    std::string& error_detail) {
  applications = rfl::Generic(rfl::Generic::Object{});

  // Helper lambdas for adding a single app or an app array
  auto add_app = [&](const std::string& app_id,
                     const rfl::Generic& app_body) -> bool {
    if (app_id.empty()) {
      error_detail = "Application identifier is missing";
      return false;
    }
    rfl::Generic normalized;
    if (!normalize_nnef_pfd_app_data(
            app_id, app_body, normalized, error_detail)) {
      return false;
    }
    auto* apps_obj = std::get_if<rfl::Generic::Object>(&applications.variant());
    (*apps_obj)[app_id] = normalized;
    return true;
  };

  auto add_app_array = [&](const rfl::Generic& app_array_gen) -> bool {
    const auto* arr =
        std::get_if<std::vector<rfl::Generic>>(&app_array_gen.variant());
    if (!arr) {
      error_detail =
          "applications must be an array when provided as a collection";
      return false;
    }
    for (const auto& app_body : *arr) {
      const auto* app_obj =
          std::get_if<rfl::Generic::Object>(&app_body.variant());
      if (!app_obj) {
        error_detail = "Each application entry must contain externalAppId";
        return false;
      }
      auto eid_it = rfl_obj_find(*app_obj, "externalAppId");
      if (eid_it == app_obj->end()) {
        error_detail = "Each application entry must contain externalAppId";
        return false;
      }
      const auto* eid_s = std::get_if<std::string>(&eid_it->second.variant());
      if (!eid_s) {
        error_detail = "Each application entry must contain externalAppId";
        return false;
      }
      if (!add_app(*eid_s, app_body)) return false;
    }
    return true;
  };

  const auto* arr = std::get_if<std::vector<rfl::Generic>>(&body.variant());
  const auto* obj = std::get_if<rfl::Generic::Object>(&body.variant());

  if (arr) {
    if (!add_app_array(body)) return false;
  } else if (obj) {
    auto apps_key_it = rfl_obj_find(*obj, "applications");
    auto pfd_key_it  = rfl_obj_find(*obj, "pfdDatas");
    auto ext_key_it  = rfl_obj_find(*obj, "externalAppId");

    if (apps_key_it != obj->end()) {
      const auto* apps_arr = std::get_if<std::vector<rfl::Generic>>(
          &apps_key_it->second.variant());
      const auto* apps_obj =
          std::get_if<rfl::Generic::Object>(&apps_key_it->second.variant());
      if (apps_arr) {
        if (!add_app_array(apps_key_it->second)) return false;
      } else if (apps_obj) {
        for (const auto& [aid, abody] : *apps_obj) {
          if (!add_app(aid, abody)) return false;
        }
      } else {
        error_detail = "applications must be an object or an array";
        return false;
      }
    } else if (pfd_key_it != obj->end()) {
      const auto* pfd_obj =
          std::get_if<rfl::Generic::Object>(&pfd_key_it->second.variant());
      if (!pfd_obj) {
        error_detail = "pfdDatas must be an object";
        return false;
      }
      for (const auto& [aid, abody] : *pfd_obj) {
        if (!add_app(aid, abody)) return false;
      }
    } else if (ext_key_it != obj->end()) {
      const auto* ext_s =
          std::get_if<std::string>(&ext_key_it->second.variant());
      if (!ext_s) {
        error_detail = "externalAppId must be a string";
        return false;
      }
      if (!add_app(*ext_s, body)) return false;
    } else {
      error_detail = "Transaction body must contain applications or pfdDatas";
      return false;
    }
  } else {
    error_detail = "Transaction body must be a JSON object or array";
    return false;
  }

  // Check not empty
  const auto* final_obj =
      std::get_if<rfl::Generic::Object>(&applications.variant());
  if (!final_obj || final_obj->empty()) {
    error_detail = "Transaction must contain at least one application PFD";
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
static rfl::Generic make_nnef_pfd_transaction(
    const std::string& transaction_id, const rfl::Generic& body,
    const rfl::Generic& applications) {
  // Start with body if it is an object, else empty object
  rfl::Generic::Object out;
  if (const auto* b_obj = std::get_if<rfl::Generic::Object>(&body.variant())) {
    out = *b_obj;
  }
  out["transactionId"] = rfl::Generic(transaction_id);
  out["applications"]  = applications;
  // Remove pfdDatas key if present (erase is available on std::map)
  rfl_obj_erase(out, "pfdDatas");
  return rfl::Generic(out);
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
  handle_nf_notification(nf_sub_id, notif.dump());
}

// Inbound notification from 5GC NF
//------------------------------------------------------------------------------
void nef_app::handle_nf_notification(
    const std::string& nf_sub_id, const std::string& notif_payload) {
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
    auto r_notif = rfl::json::read<rfl::Generic>(notif_payload);
    if (!r_notif) {
      Logger::nef_app().warn(
          "handle_nf_notification: failed to parse payload for sub-id %s",
          nf_sub_id.c_str());
      return;
    }
    rfl::Generic rfl_notif = r_notif.value();
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

    const uint8_t http_ver   = sub->get_http_version();
    auto nef_client          = m_nef_client;
    const std::string t8_str = rfl::json::write(rfl_t8);
    const bool enqueued =
        m_notification_pool->enqueue([nef_client, af_uri, t8_str, http_ver]() {
          if (!nef_client->forward_notification_to_af(
                  af_uri, nlohmann::json::parse(t8_str), http_ver)) {
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
  (void) http_version;

  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }

  std::string error_detail;
  if (!validate_nnef_event_exposure_subscription(rfl_body, error_detail)) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    return;
  }

  std::string subscription_id;
  generate_af_subscription_id(subscription_id);

  rfl::Generic stored_subscription = rfl_body;
  finalize_nnef_event_exposure_subscription(
      stored_subscription, subscription_id);

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_event_subscriptions_mutex);
    m_nnef_event_subscriptions[subscription_id] = stored_subscription;
  }

  Logger::nef_app().info(
      "Created Nnef_EventExposure subscription: %s", subscription_id.c_str());

  http_code    = http_status_code::CREATED;
  rfl_response = stored_subscription;
  nef_audit::log("CREATE", "EE", "", subscription_id, http_code);
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
  (void) http_version;

  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }

  std::shared_lock lock(m_nnef_event_subscriptions_mutex);
  auto it = m_nnef_event_subscriptions.find(subscription_id);
  if (it == m_nnef_event_subscriptions.end()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Nnef_EventExposure subscription not found");
    return;
  }

  http_code    = http_status_code::OK;
  rfl_response = it->second;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_event_exposure_update(
    const std::string& subscription_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  (void) http_version;

  if (!authorize_nnef_request(NEF_SERVICE_MONITORING_EVENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }

  std::string error_detail;
  if (!validate_nnef_event_exposure_subscription(rfl_body, error_detail)) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    return;
  }

  rfl::Generic updated_subscription = rfl_body;
  finalize_nnef_event_exposure_subscription(
      updated_subscription, subscription_id);

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_event_subscriptions_mutex);
    auto it = m_nnef_event_subscriptions.find(subscription_id);
    if (it == m_nnef_event_subscriptions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "Nnef_EventExposure subscription not found");
      return;
    }

    it->second = updated_subscription;
  }

  Logger::nef_app().info(
      "Updated Nnef_EventExposure subscription: %s", subscription_id.c_str());

  http_code    = http_status_code::OK;
  rfl_response = updated_subscription;
  nef_audit::log("UPDATE", "EE", "", subscription_id, http_code);
}

// Monitoring Event Exposure (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_create(
    const std::string& scs_as_id, const rfl::Generic& rfl_body,
    std::string& sub_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  Logger::nef_app().info(
      "Create monitoring event subscription for SCS/AS: %s", scs_as_id.c_str());

  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Validate required fields
  if (!body_for_val.contains("monitoringType") ||
      !body_for_val.contains("notificationDestination")) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
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
      err = validate_enum_field(
          body_for_val, "monitoringType", kMonitoringTypes, true);
    if (err.empty())
      err = validate_string_field(
          body_for_val, "notificationDestination", true, 2048);
    if (!err.empty()) {
      http_code    = http_status_code::UNPROCESSABLE_ENTITY;
      rfl_response = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate the callback URI before any storage or southbound
  // calls
  {
    const std::string uri_err = validate_callback_uri(
        body_for_val["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      http_code    = http_status_code::BAD_REQUEST;
      rfl_response = make_problem_detail(
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
  sub->set_http_version(http_version);
  sub->set_subscription_data(body_for_val);
  if (body_for_val.contains("notificationDestination")) {
    sub->set_notification_uri(
        body_for_val["notificationDestination"].get<std::string>());
  }
  if (body_for_val.contains("monitorExpireTime") &&
      body_for_val["monitorExpireTime"].is_string()) {
    std::chrono::system_clock::time_point expire_time;
    if (!parse_monitor_expire_time(
            body_for_val["monitorExpireTime"].get<std::string>(),
            expire_time)) {
      http_code    = http_status_code::BAD_REQUEST;
      rfl_response = make_problem_detail(
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
  if (!m_nef_client->subscribe_amf_event_exposure(
          body_for_val, amf_sub_id, http_version)) {
    Logger::nef_app().warn("Failed to subscribe to AMF event exposure");
    remove_subscription(sub_id);
    release_af_profile_subscription(scs_as_id, sub_id);
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to create AMF monitoring subscription");
    return;
  }

  sub->set_nf_subscription_id(amf_sub_id);
  if (!amf_sub_id.empty()) {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[amf_sub_id] = sub_id;
  }

  rfl_response = rfl_body;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["subId"] = rfl::Generic(sub_id);
  }
  http_code = http_status_code::CREATED;
  nef_audit::log("CREATE", "ME", scs_as_id, sub_id, http_code);
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
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_MONITORING_EVENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // GET without sub-id is treated as list operation.
  if (sub_id.empty()) {
    std::shared_lock lock(m_af_subscriptions_mutex);
    std::vector<rfl::Generic> result_arr;
    for (const auto& [id, sub] : m_af_sub_id2subscription) {
      if (!sub) continue;
      if (sub->get_service_type() !=
          nef_service_type_t::NEF_SERVICE_TYPE_MONITORING_EVENT) {
        continue;
      }
      if (sub->get_scs_as_id() != scs_as_id) continue;

      auto rfl_r =
          rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
      rfl::Generic entry =
          rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
      if (auto* e_obj = std::get_if<rfl::Generic::Object>(&entry.variant())) {
        (*e_obj)["subId"] = rfl::Generic(id);
      }
      result_arr.push_back(std::move(entry));
    }
    http_code    = http_status_code::OK;
    rfl_response = rfl::Generic(result_arr);
    return;
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "Subscription not found");
    return;
  }

  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }

  {
    auto rfl_r =
        rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
    rfl_response = rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
  }
  http_code = http_status_code::OK;
}

// Traffic Influence (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_create(
    const std::string& af_id, const rfl::Generic& rfl_body, std::string& ti_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  Logger::nef_app().info("Create TI subscription for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Validate required fields: at least one traffic filter must be present
  if (!body_for_val.contains("afAppId") &&
      !body_for_val.contains("trafficFilters") &&
      !body_for_val.contains("ethTrafficFilters")) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
        "required");
    return;
  }

  // Path-parameter and field validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty())
      err = validate_string_field(body_for_val, "afAppId", false, 256);
    if (err.empty())
      err = validate_string_field(body_for_val, "dnn", false, 100);
    if (err.empty())
      err = validate_string_field(
          body_for_val, "notificationDestination", false, 2048);
    if (!err.empty()) {
      http_code    = http_status_code::UNPROCESSABLE_ENTITY;
      rfl_response = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate notification callback URI if provided
  if (body_for_val.contains("notificationDestination") &&
      body_for_val["notificationDestination"].is_string()) {
    const std::string uri_err = validate_callback_uri(
        body_for_val["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      http_code    = http_status_code::BAD_REQUEST;
      rfl_response = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request",
          "notificationDestination: " + uri_err);
      return;
    }
  }

  generate_af_subscription_id(ti_id);

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    m_ti_sessions[ti_id] = rfl_body;
    m_ti_id2af_id[ti_id] = af_id;
  }

  // Register nef_subscription so PCF notifications can be routed back to AF
  auto ti_sub = std::make_shared<nef_subscription>(m_event_sub);
  ti_sub->set_af_subscription_id(ti_id);
  ti_sub->set_scs_as_id(af_id);
  ti_sub->set_service_type(
      nef_service_type_t::NEF_SERVICE_TYPE_TRAFFIC_INFLUENCE);
  ti_sub->set_http_version(http_version);
  ti_sub->set_subscription_data(body_for_val);
  if (body_for_val.contains("notificationDestination") &&
      body_for_val["notificationDestination"].is_string()) {
    ti_sub->set_notification_uri(
        body_for_val["notificationDestination"].get<std::string>());
  }
  add_subscription(ti_id, ti_sub);

  std::string pcf_policy_id;
  uint32_t http_code_pcf = 0;
  const bool pcf_ok      = m_nef_client->create_pcf_policy_auth(
      body_for_val, pcf_policy_id, http_code_pcf, http_version);
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
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
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
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
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
  if (!m_nef_client->udr_put_influence_data(
          ti_id, body_for_val, http_code_udr, http_version)) {
    Logger::nef_app().warn(
        "UDR influence PUT failed for ti_id=%s (http=%u)", ti_id.c_str(),
        http_code_udr);
  }

  rfl_response = rfl_body;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["afTransId"] = rfl::Generic(ti_id);
  }
  http_code = http_status_code::CREATED;
  nef_audit::log("CREATE", "TI", af_id, ti_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_traffic_influence_update(
    const std::string& af_id, const std::string& ti_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  if (!body_for_val.contains("afAppId") &&
      !body_for_val.contains("trafficFilters") &&
      !body_for_val.contains("ethTrafficFilters")) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "At least one of afAppId, trafficFilters, or ethTrafficFilters is "
        "required");
    return;
  }

  // Type and length validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty())
      err = validate_string_field(body_for_val, "afAppId", false, 256);
    if (err.empty())
      err = validate_string_field(body_for_val, "dnn", false, 100);
    if (err.empty())
      err = validate_string_field(
          body_for_val, "notificationDestination", false, 2048);
    if (!err.empty()) {
      http_code    = http_status_code::UNPROCESSABLE_ENTITY;
      rfl_response = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate notification callback URI if provided in update
  if (body_for_val.contains("notificationDestination") &&
      body_for_val["notificationDestination"].is_string()) {
    const std::string uri_err = validate_callback_uri(
        body_for_val["notificationDestination"].get<std::string>());
    if (!uri_err.empty()) {
      http_code    = http_status_code::BAD_REQUEST;
      rfl_response = make_problem_detail(
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
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      return;
    }

    auto owner_it = m_ti_id2af_id.find(ti_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code    = http_status_code::FORBIDDEN;
      rfl_response = make_problem_detail(
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
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF policy identifier for TI session");
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_policy_auth(
          pcf_policy_id, body_for_val, http_code_pcf, http_version)) {
    Logger::nef_app().warn(
        "PCF TI update failed for ti_id=%s policy_id=%s (http=%u)",
        ti_id.c_str(), pcf_policy_id.c_str(), http_code_pcf);
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update policy authorization in PCF");
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(ti_id);
    if (session_it == m_ti_sessions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      return;
    }
    session_it->second = rfl_body;
  }

  rfl_response = rfl_body;
  http_code    = http_status_code::OK;
  nef_audit::log("UPDATE", "TI", af_id, ti_id, http_code);
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
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  Logger::nef_app().info("PFD create for app: %s", app_id.c_str());

  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  if (!body_for_val.contains("pfdDatas")) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: pfdDatas");
    return;
  }

  // Path-parameter and type validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(app_id, "appId", 256);
    if (err.empty())
      err = validate_object_field(body_for_val, "pfdDatas", false);
    if (!err.empty()) {
      http_code    = http_status_code::UNPROCESSABLE_ENTITY;
      rfl_response = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, body_for_val, http_version)) {
    Logger::nef_app().warn("UDR PFD push failed for app: %s", app_id.c_str());
  }
  rfl_response = rfl_body;
  http_code    = http_status_code::CREATED;
  nef_audit::log("CREATE", "PFD", app_id, app_id, http_code);
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
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  nlohmann::json result;
  uint32_t http_code_udr = 0;
  m_nef_client->udr_get_pfd_data(app_id, result, http_code_udr);

  if (http_code_udr == http_status_code::OK) {
    auto rfl_r   = rfl::json::read<rfl::Generic>(result.dump());
    rfl_response = rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
    http_code    = http_status_code::OK;
    return;
  }

  if (http_code_udr == http_status_code::NOT_FOUND) {
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD data not found");
    http_code = http_status_code::NOT_FOUND;
    return;
  }

  rfl_response = make_problem_detail(
      http_status_code::BAD_GATEWAY, "Bad Gateway",
      "UDR returned an unexpected response for PFD GET");
  http_code = http_status_code::BAD_GATEWAY;
}

// BDT Policy (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_create(
    const std::string& af_id, const rfl::Generic& rfl_body, std::string& bdt_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  Logger::nef_app().info("BDT policy create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  if (!body_for_val.contains("bdtPolData")) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: bdtPolData");
    return;
  }

  // Path-parameter and type validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty())
      err = validate_object_field(body_for_val, "bdtPolData", false);
    if (!err.empty()) {
      http_code    = http_status_code::UNPROCESSABLE_ENTITY;
      rfl_response = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  generate_af_subscription_id(bdt_id);

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    m_bdt_sessions[bdt_id] = rfl_body;
    m_bdt_id2af_id[bdt_id] = af_id;
  }

  std::string pcf_bdt_id;
  uint32_t http_code_pcf = 0;
  const bool pcf_ok      = m_nef_client->create_pcf_bdt_policy(
      body_for_val, pcf_bdt_id, http_code_pcf, http_version);
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
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to create BDT policy in PCF");
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    m_bdt_id2pcf_policy_id[bdt_id] = pcf_bdt_id;
  }

  rfl_response = rfl_body;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["bdtRefId"] = rfl::Generic(bdt_id);
  }
  http_code = http_status_code::CREATED;
  nef_audit::log("CREATE", "BDT", af_id, bdt_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_update(
    const std::string& af_id, const std::string& bdt_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  if (!body_for_val.contains("bdtPolData")) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: bdtPolData");
    return;
  }

  std::string pcf_bdt_id;
  {
    std::shared_lock lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      return;
    }

    auto owner_it = m_bdt_id2af_id.find(bdt_id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      http_code    = http_status_code::FORBIDDEN;
      rfl_response = make_problem_detail(
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
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF BDT policy identifier");
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_bdt_policy(
          pcf_bdt_id, body_for_val, http_code_pcf, http_version)) {
    Logger::nef_app().warn(
        "PCF BDT update failed for bdt_id=%s policy_id=%s (http=%u)",
        bdt_id.c_str(), pcf_bdt_id.c_str(), http_code_pcf);
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update BDT policy in PCF");
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_bdt_mutex);
    auto session_it = m_bdt_sessions.find(bdt_id);
    if (session_it == m_bdt_sessions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
      return;
    }
    session_it->second = rfl_body;
  }

  rfl_response = rfl_body;
  http_code    = http_status_code::OK;
  nef_audit::log("UPDATE", "BDT", af_id, bdt_id, http_code);
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
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  std::shared_lock lock(m_bdt_mutex);
  std::vector<rfl::Generic> result_arr;
  for (const auto& [id, session] : m_bdt_sessions) {
    auto owner_it = m_bdt_id2af_id.find(id);
    if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
      continue;
    }
    rfl::Generic entry = session;
    if (auto* e_obj = std::get_if<rfl::Generic::Object>(&entry.variant())) {
      (*e_obj)["bdtRefId"] = rfl::Generic(id);
    }
    result_arr.push_back(std::move(entry));
  }
  http_code    = http_status_code::OK;
  rfl_response = rfl::Generic(result_arr);
}

//------------------------------------------------------------------------------
void nef_app::handle_bdt_policy_get(
    const std::string& af_id, const std::string& bdt_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_BDT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  std::shared_lock lock(m_bdt_mutex);
  auto it = m_bdt_sessions.find(bdt_id);
  if (it == m_bdt_sessions.end()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "BDT policy not found");
    return;
  }

  auto owner_it = m_bdt_id2af_id.find(bdt_id);
  if (owner_it == m_bdt_id2af_id.end() || owner_it->second != af_id) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this resource");
    return;
  }

  rfl_response = it->second;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["bdtRefId"] = rfl::Generic(bdt_id);
  }
  http_code = http_status_code::OK;
}

// QoS Provisioning (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_create(
    const std::string& af_id, const rfl::Generic& rfl_body,
    std::string& qos_sub_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  Logger::nef_app().info("QoS subscription create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  // Validate required fields
  if (!body_for_val.contains("notifUri") ||
      (!body_for_val.contains("flowInfo") &&
       !body_for_val.contains("ethFlowInfo"))) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "notifUri and at least one of flowInfo or ethFlowInfo are required");
    return;
  }

  // Path-parameter and field validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty())
      err = validate_string_field(body_for_val, "notifUri", true, 2048);
    if (err.empty())
      err = validate_string_field(body_for_val, "afAppId", false, 256);
    if (!err.empty()) {
      http_code    = http_status_code::UNPROCESSABLE_ENTITY;
      rfl_response = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate the callback URI before any storage or southbound
  // calls
  {
    const std::string uri_err =
        validate_callback_uri(body_for_val["notifUri"].get<std::string>());
    if (!uri_err.empty()) {
      http_code    = http_status_code::BAD_REQUEST;
      rfl_response = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
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
  sub->set_subscription_data(body_for_val);

  if (body_for_val.contains("requestExpiry") &&
      body_for_val["requestExpiry"].is_string()) {
    std::chrono::system_clock::time_point expire_time;
    if (parse_monitor_expire_time(
            body_for_val["requestExpiry"].get<std::string>(), expire_time)) {
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
      body_for_val, smf_sub_id, http_version);
  if (!smf_ok || smf_sub_id.empty()) {
    remove_subscription(qos_sub_id);
    release_af_profile_subscription(af_id, qos_sub_id);
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to create SMF event exposure subscription");
    return;
  }

  sub->set_nf_subscription_id(smf_sub_id);

  if (!smf_sub_id.empty()) {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[smf_sub_id] = qos_sub_id;
  }
  if (body_for_val.contains("notifUri")) {
    sub->set_notification_uri(body_for_val["notifUri"].get<std::string>());
  }

  rfl_response = rfl_body;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["subId"] = rfl::Generic(qos_sub_id);
  }
  http_code = http_status_code::CREATED;
  nef_audit::log("CREATE", "QOS", af_id, qos_sub_id, http_code);
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
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  auto sub = find_subscription(qos_sub_id);
  if (!sub) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "QoS subscription not found");
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }

  auto rfl_r =
      rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
  http_code    = http_status_code::OK;
  rfl_response = rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
}

//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_list(
    const std::string& af_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  std::shared_lock lock(m_af_subscriptions_mutex);
  std::vector<rfl::Generic> result_arr;
  for (const auto& [id, sub] : m_af_sub_id2subscription) {
    if (sub->get_service_type() ==
            nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING &&
        sub->get_scs_as_id() == af_id) {
      auto rfl_r =
          rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
      rfl::Generic entry =
          rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
      if (auto* e_obj = std::get_if<rfl::Generic::Object>(&entry.variant())) {
        (*e_obj)["subId"] = rfl::Generic(id);
      }
      result_arr.push_back(std::move(entry));
    }
  }
  http_code    = http_status_code::OK;
  rfl_response = rfl::Generic(result_arr);
}

// Analytics Subscription (TS 29.122) API handlers
//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_create(
    const std::string& af_id, const rfl::Generic& rfl_body,
    std::string& analytics_sub_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  Logger::nef_app().info(
      "Analytics subscription create for AF: %s", af_id.c_str());

  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  if (!body_for_val.contains("analyEventsSubs") ||
      !body_for_val.contains("notifUri") || !body_for_val.contains("notifId")) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "analyEventsSubs, notifUri, and notifId are required");
    return;
  }

  // Path-parameter and field validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty()) err = validate_string_param(af_id, "afId", 256);
    if (err.empty())
      err = validate_string_field(body_for_val, "notifUri", true, 2048);
    if (err.empty())
      err = validate_string_field(body_for_val, "notifId", true, 256);
    if (err.empty())
      err = validate_array_field(body_for_val, "analyEventsSubs", false, 1);
    if (!err.empty()) {
      http_code    = http_status_code::UNPROCESSABLE_ENTITY;
      rfl_response = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }

  // SSRF protection: validate the callback URI before any storage
  {
    const std::string uri_err =
        validate_callback_uri(body_for_val["notifUri"].get<std::string>());
    if (!uri_err.empty()) {
      http_code    = http_status_code::BAD_REQUEST;
      rfl_response = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
      return;
    }
  }

  generate_af_subscription_id(analytics_sub_id);
  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(analytics_sub_id);
  sub->set_scs_as_id(af_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_ANALYTICS);
  sub->set_http_version(http_version);
  sub->set_subscription_data(body_for_val);

  add_subscription(analytics_sub_id, sub);
  ensure_af_profile(af_id, analytics_sub_id);
  rfl_response = rfl_body;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["subId"] = rfl::Generic(analytics_sub_id);
  }
  http_code = http_status_code::CREATED;
  nef_audit::log("CREATE", "ANA", af_id, analytics_sub_id, http_code);
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
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  auto sub = find_subscription(analytics_sub_id);
  if (!sub) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Analytics subscription not found");
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }

  auto rfl_r =
      rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
  http_code    = http_status_code::OK;
  rfl_response = rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
}

//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_list(
    const std::string& af_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(af_id, NEF_SERVICE_ANALYTICS)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  std::shared_lock lock(m_af_subscriptions_mutex);
  std::vector<rfl::Generic> result_arr;
  for (const auto& [id, sub] : m_af_sub_id2subscription) {
    if (sub->get_service_type() ==
            nef_service_type_t::NEF_SERVICE_TYPE_ANALYTICS &&
        sub->get_scs_as_id() == af_id) {
      auto rfl_r =
          rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
      rfl::Generic entry =
          rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
      if (auto* e_obj = std::get_if<rfl::Generic::Object>(&entry.variant())) {
        (*e_obj)["subId"] = rfl::Generic(id);
      }
      result_arr.push_back(std::move(entry));
    }
  }
  http_code    = http_status_code::OK;
  rfl_response = rfl::Generic(result_arr);
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
        const auto* sub_obj = std::get_if<rfl::Generic::Object>(&sub.variant());
        if (!sub_obj) continue;
        auto rep_it = rfl_obj_find(*sub_obj, "eventsRepInfo");
        if (rep_it == sub_obj->end()) continue;
        const auto* rep_obj =
            std::get_if<rfl::Generic::Object>(&rep_it->second.variant());
        if (!rep_obj) continue;
        auto dur_it = rfl_obj_find(*rep_obj, "monDur");
        if (dur_it == rep_obj->end()) continue;
        const auto* dur_str =
            std::get_if<std::string>(&dur_it->second.variant());
        if (!dur_str) continue;
        std::chrono::system_clock::time_point expire_tp;
        if (!parse_monitor_expire_time(*dur_str, expire_tp)) continue;
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
  if (!authorize_af_request(af_id, NEF_SERVICE_TRAFFIC_INFLUENCE)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  std::string pcf_policy_id;
  rfl::Generic r_result;
  nlohmann::json patched_copy;
  {
    std::shared_lock lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(app_session_id);
    if (session_it == m_ti_sessions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      return;
    }
    auto owner_it = m_ti_id2af_id.find(app_session_id);
    if (owner_it == m_ti_id2af_id.end() || owner_it->second != af_id) {
      http_code    = http_status_code::FORBIDDEN;
      rfl_response = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }
    auto pcf_it = m_ti_id2pcf_policy_id.find(app_session_id);
    if (pcf_it != m_ti_id2pcf_policy_id.end()) {
      pcf_policy_id = pcf_it->second;
    }
    // session_it->second is already rfl::Generic — merge directly
    r_result     = nef_merge_patch(session_it->second, patch_body);
    patched_copy = nlohmann::json::parse(rfl::json::write(r_result));
  }

  if (pcf_policy_id.empty()) {
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Missing PCF policy identifier for TI session");
    return;
  }

  uint32_t http_code_pcf = 0;
  if (!m_nef_client->update_pcf_policy_auth(
          pcf_policy_id, patched_copy, http_code_pcf, http_version)) {
    Logger::nef_app().warn(
        "PCF TI patch failed for ti_id=%s (http=%u)", app_session_id.c_str(),
        http_code_pcf);
    http_code    = http_status_code::BAD_GATEWAY;
    rfl_response = make_problem_detail(
        http_status_code::BAD_GATEWAY, "Bad Gateway",
        "Failed to update policy authorization in PCF");
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
    auto session_it = m_ti_sessions.find(app_session_id);
    if (session_it == m_ti_sessions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found", "TI session not found");
      return;
    }
    session_it->second = r_result;
  }

  rfl_response = r_result;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["afTransId"] = rfl::Generic(app_session_id);
  }
  http_code = http_status_code::OK;
  nef_audit::log("PATCH", "TI", af_id, app_session_id, http_code);
}

// QoS PATCH handler (TS 29.122)
//------------------------------------------------------------------------------
void nef_app::handle_qos_subscription_patch(
    const std::string& scs_as_id, const std::string& sub_id,
    const rfl::Generic& patch_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_QOS_MONITORING)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "QoS subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }
  auto r_base =
      rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
  if (!r_base) {
    http_code    = http_status_code::INTERNAL_SERVER_ERROR;
    rfl_response = make_problem_detail(
        http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
        "Failed to parse stored QoS subscription");
    return;
  }
  const rfl::Generic r_result = nef_merge_patch(r_base.value(), patch_body);
  nlohmann::json patched = nlohmann::json::parse(rfl::json::write(r_result));
  // SSRF protection: validate callback URI in the patched result if present
  if (patched.contains("notifUri") && patched["notifUri"].is_string()) {
    const std::string uri_err =
        validate_callback_uri(patched["notifUri"].get<std::string>());
    if (!uri_err.empty()) {
      http_code    = http_status_code::BAD_REQUEST;
      rfl_response = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
      return;
    }
  }
  sub->set_subscription_data(patched);
  if (patched.contains("notifUri")) {
    sub->set_notification_uri(patched["notifUri"].get<std::string>());
  }
  rfl_response = r_result;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["subId"] = rfl::Generic(sub_id);
  }
  http_code = http_status_code::OK;
  nef_audit::log("PATCH", "QOS", scs_as_id, sub_id, http_code);
}

// PFD transaction-level and app-level endpoints (TS 29.122)
//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_list(
    const std::string& scs_as_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  std::shared_lock lock(m_pfd_mutex);
  std::vector<rfl::Generic> result_arr;
  for (const auto& [tid, session] : m_pfd_trans_sessions) {
    auto owner_it = m_pfd_trans2scs_id.find(tid);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id)
      continue;
    rfl::Generic entry = session;
    if (auto* e_obj = std::get_if<rfl::Generic::Object>(&entry.variant())) {
      (*e_obj)["transId"] = rfl::Generic(tid);
    }
    result_arr.push_back(std::move(entry));
  }
  http_code    = http_status_code::OK;
  rfl_response = rfl::Generic(result_arr);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  if (!body_for_val.contains("pfdDatas")) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "Missing required field: pfdDatas");
    return;
  }

  // Type validation (422 for semantic errors).
  if (!body_for_val["pfdDatas"].is_object()) {
    http_code    = http_status_code::UNPROCESSABLE_ENTITY;
    rfl_response = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        "pfdDatas: must be an object");
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
  for (auto& [app_id, pfd_data] : body_for_val["pfdDatas"].items()) {
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
      http_code    = http_status_code::INTERNAL_SERVER_ERROR;
      rfl_response = make_problem_detail(
          http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
          "PFD transaction aborted: UDR write failed for app " + app_id);
      return;
    }
    pfd_rollback.mark_committed(app_id);
  }

  // All UDR writes succeeded — commit local state (deferred commit)
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    m_pfd_trans_sessions[trans_id] = rfl_body;
    m_pfd_trans2scs_id[trans_id]   = scs_as_id;
  }

  rfl_response = rfl_body;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["transId"] = rfl::Generic(trans_id);
  }
  http_code = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "PFD_TX", scs_as_id, trans_id,
      http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id, int& http_code,
    uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }
  rfl::Generic trans_body;
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
  if (const auto* trans_obj =
          std::get_if<rfl::Generic::Object>(&trans_body.variant())) {
    auto pfd_it = rfl_obj_find(*trans_obj, "pfdDatas");
    if (pfd_it != trans_obj->end()) {
      if (const auto* pfd_obj =
              std::get_if<rfl::Generic::Object>(&pfd_it->second.variant())) {
        for (const auto& [app_id, _] : *pfd_obj) {
          m_nef_client->udr_delete_pfd_data(app_id, http_version);
        }
      }
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
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  std::shared_lock lock(m_pfd_mutex);
  auto it = m_pfd_trans_sessions.find(trans_id);
  if (it == m_pfd_trans_sessions.end()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD transaction not found");
    return;
  }
  auto owner_it = m_pfd_trans2scs_id.find(trans_id);
  if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this resource");
    return;
  }
  const auto* trans_obj =
      std::get_if<rfl::Generic::Object>(&it->second.variant());
  const rfl::Generic::Object* pfd_obj_ptr = nullptr;
  rfl::Generic::Object empty_pfd;
  if (trans_obj) {
    auto pfd_it = rfl_obj_find(*trans_obj, "pfdDatas");
    if (pfd_it != trans_obj->end()) {
      pfd_obj_ptr =
          std::get_if<rfl::Generic::Object>(&pfd_it->second.variant());
    }
  }
  const rfl::Generic::Object& pfd_datas =
      pfd_obj_ptr ? *pfd_obj_ptr : empty_pfd;
  auto app_it = rfl_obj_find(pfd_datas, app_id);
  if (app_it == pfd_datas.cend()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Application PFD not found in transaction");
    return;
  }
  rfl_response = app_it->second;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["appId"] = rfl::Generic(app_id);
  }
  http_code = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  bool is_create = false;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "PFD transaction not found");
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code    = http_status_code::FORBIDDEN;
      rfl_response = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }
    // Navigate to pfdDatas within the stored rfl::Generic
    auto* trans_obj = std::get_if<rfl::Generic::Object>(&it->second.variant());
    if (!trans_obj) {
      // Initialize as an empty object if needed
      it->second = rfl::Generic(rfl::Generic::Object{});
      trans_obj  = std::get_if<rfl::Generic::Object>(&it->second.variant());
    }
    auto pfd_it = rfl_obj_find(*trans_obj, "pfdDatas");
    if (pfd_it == trans_obj->end() ||
        !std::get_if<rfl::Generic::Object>(&pfd_it->second.variant())) {
      (*trans_obj)["pfdDatas"] = rfl::Generic(rfl::Generic::Object{});
      pfd_it                   = rfl_obj_find(*trans_obj, "pfdDatas");
    }
    auto* pfd_obj =
        std::get_if<rfl::Generic::Object>(&pfd_it->second.variant());
    is_create          = (rfl_obj_find(*pfd_obj, app_id) == pfd_obj->end());
    (*pfd_obj)[app_id] = rfl_body;
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, body_for_val, http_version)) {
    Logger::nef_app().warn(
        "UDR PFD app PUT failed for app: %s", app_id.c_str());
  }

  rfl_response = rfl_body;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["appId"] = rfl::Generic(app_id);
  }
  http_code = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "PFD_APP", scs_as_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const rfl::Generic& patch_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }

  rfl::Generic r_result;
  nlohmann::json patched;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "PFD transaction not found");
      return;
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      http_code    = http_status_code::FORBIDDEN;
      rfl_response = make_problem_detail(
          http_status_code::FORBIDDEN, "Forbidden",
          "AF is not allowed to access this resource");
      return;
    }
    auto* trans_obj = std::get_if<rfl::Generic::Object>(&it->second.variant());
    const rfl::Generic::Object* pfd_obj_ptr = nullptr;
    rfl::Generic::Object empty_pfd;
    if (trans_obj) {
      auto pfd_it = rfl_obj_find(*trans_obj, "pfdDatas");
      if (pfd_it != trans_obj->end()) {
        pfd_obj_ptr =
            std::get_if<rfl::Generic::Object>(&pfd_it->second.variant());
      }
    }
    const rfl::Generic::Object& pfd_datas =
        pfd_obj_ptr ? *pfd_obj_ptr : empty_pfd;
    auto app_it = rfl_obj_find(pfd_datas, app_id);
    if (app_it == pfd_datas.cend()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "Application PFD not found in transaction");
      return;
    }
    r_result = nef_merge_patch(app_it->second, patch_body);
    patched  = nlohmann::json::parse(rfl::json::write(r_result));
    // Store back
    if (trans_obj) {
      auto pfd_it = rfl_obj_find(*trans_obj, "pfdDatas");
      if (pfd_it != trans_obj->end()) {
        auto* pfd_obj_mut =
            std::get_if<rfl::Generic::Object>(&pfd_it->second.variant());
        if (pfd_obj_mut) (*pfd_obj_mut)[app_id] = r_result;
      }
    }
  }

  if (!m_nef_client->udr_put_pfd_data(app_id, patched, http_version)) {
    Logger::nef_app().warn(
        "UDR PFD app PATCH failed for app: %s", app_id.c_str());
  }

  rfl_response = r_result;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["appId"] = rfl::Generic(app_id);
  }
  http_code = http_status_code::OK;
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
    auto* trans_obj = std::get_if<rfl::Generic::Object>(&it->second.variant());
    if (!trans_obj) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    auto pfd_it = rfl_obj_find(*trans_obj, "pfdDatas");
    if (pfd_it == trans_obj->end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    auto* pfd_obj =
        std::get_if<rfl::Generic::Object>(&pfd_it->second.variant());
    if (!pfd_obj || rfl_obj_find(*pfd_obj, app_id) == pfd_obj->end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    rfl_obj_erase(*pfd_obj, app_id);
  }
  m_nef_client->udr_delete_pfd_data(app_id, http_version);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "PFD_APP", scs_as_id, app_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_list_transactions(
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  std::vector<rfl::Generic> result_arr;
  for (const auto& [transaction_id, transaction] : m_nnef_pfd_transactions) {
    result_arr.push_back(transaction);
  }
  http_code    = http_status_code::OK;
  rfl_response = rfl::Generic(result_arr);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_put_transaction(
    const std::string& transaction_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  rfl::Generic applications;
  std::string error_detail;
  if (!extract_nnef_pfd_transaction_apps(
          rfl_body, applications, error_detail)) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    return;
  }

  rfl::Generic transaction =
      make_nnef_pfd_transaction(transaction_id, rfl_body, applications);

  // Check create-vs-update before UDR writes (deferred local commit)
  bool is_create;
  rfl::Generic previous_transaction;
  {
    std::shared_lock lock(m_nnef_pfd_transactions_mutex);
    auto it   = m_nnef_pfd_transactions.find(transaction_id);
    is_create = (it == m_nnef_pfd_transactions.end());
    if (!is_create) {
      previous_transaction = it->second;
    }
  }

  const auto* apps_obj =
      std::get_if<rfl::Generic::Object>(&applications.variant());

  // Write each app to UDR atomically — rollback committed apps on failure
  PfdRollbackTracker pfd_rollback;
  if (apps_obj) {
    for (const auto& [app_id, app_body] : *apps_obj) {
      if (!m_nef_client->udr_put_pfd_data(
              app_id, nlohmann::json::parse(rfl::json::write(app_body)),
              http_version)) {
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
              "%d rollback failure(s) in Nnef trans '%s' — UDR may retain "
              "orphan data",
              rb_failures, transaction_id.c_str());
        }
        http_code    = http_status_code::INTERNAL_SERVER_ERROR;
        rfl_response = make_problem_detail(
            http_status_code::INTERNAL_SERVER_ERROR, "Internal Server Error",
            "PFD transaction aborted: UDR write failed for app " + app_id);
        return;
      }
      pfd_rollback.mark_committed(app_id);
    }
  }

  // All UDR writes succeeded — commit local state
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    m_nnef_pfd_transactions[transaction_id] = transaction;
  }

  // Delete apps removed in update
  if (!is_create) {
    if (const auto* prev_obj = std::get_if<rfl::Generic::Object>(
            &previous_transaction.variant())) {
      auto prev_apps_it = rfl_obj_find(*prev_obj, "applications");
      if (prev_apps_it != prev_obj->end()) {
        if (const auto* prev_apps = std::get_if<rfl::Generic::Object>(
                &prev_apps_it->second.variant())) {
          for (const auto& [app_id, _] : *prev_apps) {
            if (apps_obj && rfl_obj_find(*apps_obj, app_id) != apps_obj->end())
              continue;
            if (!m_nef_client->udr_delete_pfd_data(app_id, http_version)) {
              Logger::nef_app().warn(
                  "UDR PFD delete failed for removed Nnef_PFDmanagement app: "
                  "%s in transaction: %s",
                  app_id.c_str(), transaction_id.c_str());
            }
          }
        }
      }
    }
  }

  http_code = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "NNEF_PFD_TX", "", transaction_id,
      http_code);

  // Notify SBI PFD subscribers about the PFD change
  if (const auto* trans_obj =
          std::get_if<rfl::Generic::Object>(&transaction.variant())) {
    auto notif_apps_it = rfl_obj_find(*trans_obj, "applications");
    if (notif_apps_it != trans_obj->end()) {
      if (const auto* notif_apps = std::get_if<rfl::Generic::Object>(
              &notif_apps_it->second.variant())) {
        for (const auto& [app_id, app_data] : *notif_apps) {
          notify_nnef_pfd_subscribers("PFD_CHANGE", app_id, app_data);
        }
      }
    }
  }
  rfl_response = transaction;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_get_transaction(
    const std::string& transaction_id, rfl::Generic& rfl_response,
    int& http_code, uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto it = m_nnef_pfd_transactions.find(transaction_id);
  if (it == m_nnef_pfd_transactions.end()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD transaction not found");
    return;
  }
  http_code    = http_status_code::OK;
  rfl_response = it->second;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_delete_transaction(
    const std::string& transaction_id, int& http_code, uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }
  rfl::Generic transaction;
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

  if (const auto* trans_obj =
          std::get_if<rfl::Generic::Object>(&transaction.variant())) {
    auto apps_it = rfl_obj_find(*trans_obj, "applications");
    if (apps_it != trans_obj->end()) {
      if (const auto* apps_obj =
              std::get_if<rfl::Generic::Object>(&apps_it->second.variant())) {
        for (const auto& [app_id, _] : *apps_obj) {
          if (!m_nef_client->udr_delete_pfd_data(app_id, http_version)) {
            Logger::nef_app().warn(
                "UDR PFD delete failed for Nnef_PFDmanagement app: %s in "
                "transaction: %s",
                app_id.c_str(), transaction_id.c_str());
          }
        }
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
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto transaction_it = m_nnef_pfd_transactions.find(transaction_id);
  if (transaction_it == m_nnef_pfd_transactions.end()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD transaction not found");
    return;
  }

  const auto* tx_obj =
      std::get_if<rfl::Generic::Object>(&transaction_it->second.variant());
  if (!tx_obj) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Application PFD not found in transaction");
    return;
  }
  auto apps_it = rfl_obj_find(*tx_obj, "applications");
  if (apps_it == tx_obj->end()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Application PFD not found in transaction");
    return;
  }
  const auto* apps_obj =
      std::get_if<rfl::Generic::Object>(&apps_it->second.variant());
  if (!apps_obj) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Application PFD not found in transaction");
    return;
  }
  auto app_it = rfl_obj_find(*apps_obj, app_id);
  if (app_it == apps_obj->end()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Application PFD not found in transaction");
    return;
  }

  http_code    = http_status_code::OK;
  rfl_response = app_it->second;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_put_app(
    const std::string& transaction_id, const std::string& app_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::string error_detail;
  rfl::Generic normalized_app;
  if (!normalize_nnef_pfd_app_data(
          app_id, rfl_body, normalized_app, error_detail)) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", error_detail);
    return;
  }

  bool is_create = false;
  rfl::Generic response_app;
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto& tx_generic = m_nnef_pfd_transactions[transaction_id];
    // Ensure transaction is an object
    if (!std::get_if<rfl::Generic::Object>(&tx_generic.variant())) {
      tx_generic = rfl::Generic(rfl::Generic::Object{});
    }
    auto* tx_obj = std::get_if<rfl::Generic::Object>(&tx_generic.variant());
    (*tx_obj)["transactionId"] = rfl::Generic(transaction_id);
    // Ensure applications sub-object exists
    auto apps_it = rfl_obj_find(*tx_obj, "applications");
    if (apps_it == tx_obj->end() ||
        !std::get_if<rfl::Generic::Object>(&apps_it->second.variant())) {
      (*tx_obj)["applications"] = rfl::Generic(rfl::Generic::Object{});
      apps_it                   = rfl_obj_find(*tx_obj, "applications");
    }
    auto* apps_obj =
        std::get_if<rfl::Generic::Object>(&apps_it->second.variant());
    is_create           = (rfl_obj_find(*apps_obj, app_id) == apps_obj->end());
    (*apps_obj)[app_id] = normalized_app;
    // Remove legacy pfdDatas key if present
    rfl_obj_erase(*tx_obj, "pfdDatas");
    response_app = normalized_app;
  }

  if (!m_nef_client->udr_put_pfd_data(
          app_id, nlohmann::json::parse(rfl::json::write(normalized_app)),
          http_version)) {
    Logger::nef_app().warn(
        "UDR PFD app PUT failed for Nnef_PFDmanagement app: %s",
        app_id.c_str());
  }

  http_code = is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "NNEF_PFD_APP", "", app_id, http_code);
  // Notify SBI PFD subscribers
  notify_nnef_pfd_subscribers("PFD_CHANGE", app_id, normalized_app);
  rfl_response = response_app;
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
    auto* tx_obj =
        std::get_if<rfl::Generic::Object>(&transaction_it->second.variant());
    if (!tx_obj) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    auto apps_it = rfl_obj_find(*tx_obj, "applications");
    if (apps_it == tx_obj->end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    auto* apps_obj =
        std::get_if<rfl::Generic::Object>(&apps_it->second.variant());
    if (!apps_obj || rfl_obj_find(*apps_obj, app_id) == apps_obj->end()) {
      http_code = http_status_code::NOT_FOUND;
      return;
    }
    rfl_obj_erase(*apps_obj, app_id);
  }

  if (!m_nef_client->udr_delete_pfd_data(app_id, http_version)) {
    Logger::nef_app().warn(
        "UDR PFD app DELETE failed for Nnef_PFDmanagement app: %s",
        app_id.c_str());
  }
  // Notify SBI PFD subscribers about removal (null sentinel)
  notify_nnef_pfd_subscribers("PFD_REMOVE", app_id, rfl::Generic(std::nullopt));
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "NNEF_PFD_APP", "", app_id, http_code);
}

// Nnef_PFDmanagement — GET /applications
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter, rfl::Generic& rfl_response,
    int& http_code, uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  // Returns all PFD apps across all transactions, filtered by app-ids param.
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  std::vector<rfl::Generic> result_arr;
  for (const auto& [trans_id, transaction] : m_nnef_pfd_transactions) {
    const auto* tx_obj =
        std::get_if<rfl::Generic::Object>(&transaction.variant());
    if (!tx_obj) continue;
    auto apps_it = rfl_obj_find(*tx_obj, "applications");
    if (apps_it == tx_obj->end()) continue;
    const auto* apps_obj =
        std::get_if<rfl::Generic::Object>(&apps_it->second.variant());
    if (!apps_obj) continue;
    for (const auto& [app_id, app_data] : *apps_obj) {
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
      rfl::Generic entry = app_data;
      if (auto* e_obj = std::get_if<rfl::Generic::Object>(&entry.variant())) {
        (*e_obj)["transId"] = rfl::Generic(trans_id);
      }
      result_arr.push_back(std::move(entry));
    }
  }
  http_code    = http_status_code::OK;
  rfl_response = rfl::Generic(result_arr);
}

// Nnef_PFDmanagement — POST /applications/partial-pull
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_partial_pull(
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  // Partial-pull: client supplies optional list of appIds and optional
  // lastQueryTime. Return matching apps (queried from UDR; fallback to local
  // cache if UDR unavailable).
  std::vector<std::string> requested_ids;
  if (const auto* req_obj =
          std::get_if<rfl::Generic::Object>(&rfl_body.variant())) {
    auto ids_it = rfl_obj_find(*req_obj, "appIds");
    if (ids_it != req_obj->end()) {
      if (const auto* ids_arr = std::get_if<std::vector<rfl::Generic>>(
              &ids_it->second.variant())) {
        for (const auto& v : *ids_arr) {
          if (const auto* s = std::get_if<std::string>(&v.variant())) {
            requested_ids.push_back(*s);
          }
        }
      }
    }
  }

  std::vector<rfl::Generic> result_arr;
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  for (const auto& [trans_id, transaction] : m_nnef_pfd_transactions) {
    const auto* tx_obj =
        std::get_if<rfl::Generic::Object>(&transaction.variant());
    if (!tx_obj) continue;
    auto apps_it = rfl_obj_find(*tx_obj, "applications");
    if (apps_it == tx_obj->end()) continue;
    const auto* apps_obj =
        std::get_if<rfl::Generic::Object>(&apps_it->second.variant());
    if (!apps_obj) continue;
    for (const auto& [app_id, app_data] : *apps_obj) {
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
      rfl::Generic entry;
      if (udr_code == http_status_code::OK) {
        auto rfl_r = rfl::json::read<rfl::Generic>(udr_result.dump());
        entry      = rfl_r ? rfl_r.value() : app_data;
      } else {
        entry = app_data;
      }
      if (auto* e_obj = std::get_if<rfl::Generic::Object>(&entry.variant())) {
        (*e_obj)["applicationId"] = rfl::Generic(app_id);
      }
      result_arr.push_back(std::move(entry));
    }
  }
  http_code    = http_status_code::OK;
  rfl_response = rfl::Generic(result_arr);
}

// Nnef_PFDmanagement — subscription CRUD
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_create(
    const rfl::Generic& rfl_body, std::string& sub_id,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  // Validate required fields via rfl::Generic::Object
  const auto* body_obj = std::get_if<rfl::Generic::Object>(&rfl_body.variant());
  const std::string* notif_uri_ptr = nullptr;
  if (body_obj) {
    auto it = rfl_obj_find(*body_obj, "notifUri");
    if (it != body_obj->end()) {
      notif_uri_ptr = std::get_if<std::string>(&it->second.variant());
    }
  }
  if (!notif_uri_ptr || notif_uri_ptr->empty()) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri is required");
    return;
  }
  const std::string uri_err = validate_callback_uri(*notif_uri_ptr);
  if (!uri_err.empty()) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
    return;
  }

  generate_af_subscription_id(sub_id);
  // Build stored as copy of rfl_body with subId field added
  rfl::Generic stored = rfl_body;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&stored.variant())) {
    (*obj)["subId"] = rfl::Generic(sub_id);
  }

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_subscriptions_mutex);
    m_nnef_pfd_subscriptions[sub_id] = stored;
  }
  http_code = http_status_code::CREATED;
  nef_audit::log("CREATE", "NNEF_PFD_SUB", "", sub_id, http_code);
  rfl_response = stored;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_get(
    const std::string& sub_id, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  std::shared_lock lock(m_nnef_pfd_subscriptions_mutex);
  auto it = m_nnef_pfd_subscriptions.find(sub_id);
  if (it == m_nnef_pfd_subscriptions.end()) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found", "PFD subscription not found");
    return;
  }
  http_code    = http_status_code::OK;
  rfl_response = it->second;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_put(
    const std::string& sub_id, const rfl::Generic& rfl_body,
    rfl::Generic& rfl_response, int& http_code, uint8_t http_version) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "NF not authorized for this Nnef service");
    return;
  }
  // Full replace semantics (PUT) — validate notifUri via rfl::Generic
  const auto* body_obj = std::get_if<rfl::Generic::Object>(&rfl_body.variant());
  const std::string* notif_uri_ptr = nullptr;
  if (body_obj) {
    auto it = rfl_obj_find(*body_obj, "notifUri");
    if (it != body_obj->end()) {
      notif_uri_ptr = std::get_if<std::string>(&it->second.variant());
    }
  }
  if (!notif_uri_ptr || notif_uri_ptr->empty()) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri is required");
    return;
  }
  const std::string uri_err = validate_callback_uri(*notif_uri_ptr);
  if (!uri_err.empty()) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
    return;
  }
  rfl::Generic stored = rfl_body;
  if (auto* obj = std::get_if<rfl::Generic::Object>(&stored.variant())) {
    (*obj)["subId"] = rfl::Generic(sub_id);
  }
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_subscriptions_mutex);
    auto it = m_nnef_pfd_subscriptions.find(sub_id);
    if (it == m_nnef_pfd_subscriptions.end()) {
      http_code    = http_status_code::NOT_FOUND;
      rfl_response = make_problem_detail(
          http_status_code::NOT_FOUND, "Not Found",
          "PFD subscription not found");
      return;
    }
    // Full replace: discard old data, write new body entirely
    it->second = stored;
  }
  http_code = http_status_code::OK;
  nef_audit::log("UPDATE", "NNEF_PFD_SUB", "", sub_id, http_code);
  rfl_response = stored;
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
    const rfl::Generic& pfd_data) {
  std::vector<std::pair<std::string, std::string>>
      targets;  // (sub_id, notifUri)
  {
    std::shared_lock lock(m_nnef_pfd_subscriptions_mutex);
    for (const auto& [sid, sub] : m_nnef_pfd_subscriptions) {
      const auto* sub_obj = std::get_if<rfl::Generic::Object>(&sub.variant());
      if (!sub_obj) continue;
      auto uri_it = std::find_if(
          sub_obj->begin(), sub_obj->end(),
          [](const auto& p) { return p.first == "notifUri"; });
      if (uri_it == sub_obj->end()) continue;
      const auto* uri_str = std::get_if<std::string>(&uri_it->second.variant());
      if (!uri_str) continue;
      // Use shared helper (checks "applicationIds" key — canonical TS 29.551
      // name) — boundary convert sub to nlohmann for this out-of-scope call
      const nlohmann::json sub_nj =
          nlohmann::json::parse(rfl::json::write(sub));
      if (!nnef_pfd_subscription_matches(sub_nj, app_id)) continue;
      targets.emplace_back(sid, *uri_str);
    }
  }

  for (const auto& [sid, notif_uri] : targets) {
    rfl::Generic::Object notif_obj;
    notif_obj["eventType"] = rfl::Generic(event_type);
    notif_obj["appId"]     = rfl::Generic(app_id);
    // Only add pfdData if it is not null
    if (!std::get_if<std::nullopt_t>(&pfd_data.variant())) {
      notif_obj["pfdData"] = pfd_data;
    }
    const std::string notif_str = rfl::json::write(rfl::Generic(notif_obj));
    m_notification_pool->enqueue([this, notif_uri, notif_str, sid]() {
      m_nef_client->forward_notification_to_af(
          notif_uri, nlohmann::json::parse(notif_str));
    });
  }
}

// Analytics UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const rfl::Generic& rfl_body, rfl::Generic& rfl_response, int& http_code,
    uint8_t http_version) {
  const nlohmann::json body_for_val =
      nlohmann::json::parse(rfl::json::write(rfl_body));
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_ANALYTICS)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF not authorized for this service");
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code    = http_status_code::NOT_FOUND;
    rfl_response = make_problem_detail(
        http_status_code::NOT_FOUND, "Not Found",
        "Analytics subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code    = http_status_code::FORBIDDEN;
    rfl_response = make_problem_detail(
        http_status_code::FORBIDDEN, "Forbidden",
        "AF is not allowed to access this subscription");
    return;
  }
  if (!body_for_val.contains("analyEventsSubs") ||
      !body_for_val.contains("notifUri") || !body_for_val.contains("notifId")) {
    http_code    = http_status_code::BAD_REQUEST;
    rfl_response = make_problem_detail(
        http_status_code::BAD_REQUEST, "Bad Request",
        "analyEventsSubs, notifUri, and notifId are required");
    return;
  }
  // Type and length validation (422 for semantic errors).
  {
    std::string err;
    if (err.empty())
      err = validate_string_field(body_for_val, "notifUri", true, 2048);
    if (err.empty())
      err = validate_string_field(body_for_val, "notifId", true, 256);
    if (err.empty() && !body_for_val["analyEventsSubs"].is_array())
      err = "analyEventsSubs: must be an array";
    if (!err.empty()) {
      http_code    = http_status_code::UNPROCESSABLE_ENTITY;
      rfl_response = make_problem_detail(
          http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity", err);
      return;
    }
  }
  // SSRF protection: validate the callback URI before updating stored state
  {
    const std::string uri_err =
        validate_callback_uri(body_for_val["notifUri"].get<std::string>());
    if (!uri_err.empty()) {
      http_code    = http_status_code::BAD_REQUEST;
      rfl_response = make_problem_detail(
          http_status_code::BAD_REQUEST, "Bad Request", "notifUri: " + uri_err);
      return;
    }
  }
  sub->set_subscription_data(body_for_val);
  if (body_for_val.contains("notifUri") &&
      body_for_val["notifUri"].is_string()) {
    sub->set_notification_uri(body_for_val["notifUri"].get<std::string>());
  }
  {
    auto rfl_r =
        rfl::json::read<rfl::Generic>(sub->get_subscription_data().dump());
    rfl_response = rfl_r ? rfl_r.value() : rfl::Generic(rfl::Generic::Object{});
  }
  if (auto* obj = std::get_if<rfl::Generic::Object>(&rfl_response.variant())) {
    (*obj)["subId"] = rfl::Generic(sub_id);
  }
  http_code = http_status_code::OK;
  nef_audit::log("UPDATE", "ANA", scs_as_id, sub_id, http_code);
}
