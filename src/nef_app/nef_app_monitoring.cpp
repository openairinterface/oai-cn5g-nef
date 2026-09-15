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

// Monitoring Event UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          scs_as_id, NEF_SERVICE_MONITORING_EVENT, response_body, http_code)) {
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN,
        "AF is not allowed to access this subscription");
    return;
  }

  // Parse + validate notificationDestination directly from JSON
  if (!body.contains("notificationDestination") ||
      !body["notificationDestination"].is_string()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
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
        http_status_code::BAD_REQUEST, "notificationDestination: " + uri_err);
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

//------------------------------------------------------------------------------
void nef_app::handle_monitoring_event_subscription_get(
    const std::string& scs_as_id, const std::string& sub_id,
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          scs_as_id, NEF_SERVICE_MONITORING_EVENT, response_body, http_code)) {
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
        http_status_code::FORBIDDEN,
        "AF is not allowed to access this subscription");
    return;
  }

  response_body = sub->get_subscription_data();
  http_code     = http_status_code::OK;
}

//------------------------------------------------------------------------------
// Phase 1. The pre-southbound work is unchanged from the sync handler. The
// AMF subscribe becomes an async fire; the rollback and the post-wiring move
// into cont_monitoring_event_subscribe.
void nef_app::monitoring_event_subscribe(
    const std::string& scs_as_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info(
      "Create monitoring event subscription for SCS/AS: %s", scs_as_id.c_str());

  if (reject_unauthorized_af(scs_as_id, NEF_SERVICE_MONITORING_EVENT, sink)) {
    return;
  }

  if (!body.contains("monitoringType") ||
      !body.contains("notificationDestination")) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
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
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, err)
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
              http_status_code::BAD_REQUEST,
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
              http_status_code::BAD_REQUEST, "Invalid monitorExpireTime format")
              .dump());
    }
    sub->set_expire_time(expire_time);
  }

  add_subscription(sub_id, sub);
  ensure_af_profile(scs_as_id, sub_id);
  clear_request_bearer_token();

  // Still on the dispatcher worker here, so letting the wrapper do its own
  // discovery is safe.
  m_nef_client->subscribe_amf_event_exposure_async(
      body, [this, scs_as_id, sub_id, body,
             sink = std::move(sink)](oai::sba::response r) mutable {
        cont_monitoring_event_subscribe(
            scs_as_id, sub_id, body, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_monitoring_event_subscribe(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_monitoring_event_subscribe on thread, sub_id=%s status=%d",
      sub_id.c_str(), r.status_code);
  const std::string amf_sub_id = sbi_ok(r) ? nef_async_parse_amf_sub_id(r) : "";
  // An AMF failure, or a 2xx carrying no usable id, rolls back the local
  // state and fails the request with 502 — same as the sync handler.
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

//------------------------------------------------------------------------------
// Phase 1. The authorize/owner block is unchanged from the sync handler.
// The AMF unsubscribe becomes an async fire and its result is not checked:
// cont_monitoring_event_unsubscribe does the local cleanup and answers 204
// regardless of the southbound outcome (best-effort, 204 unconditionally).
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

  // Fire the AMF unsubscribe. With no NF sub id there is nothing to
  // unsubscribe, so finish the cleanup inline -- the sync path skips the
  // southbound call too.
  if (nf_sub_id.empty()) {
    return cont_monitoring_event_unsubscribe(
        scs_as_id, sub_id, nf_sub_id, oai::sba::response{}, std::move(sink));
  }
  m_nef_client->unsubscribe_amf_event_exposure_async(
      nf_sub_id, [this, scs_as_id, sub_id, nf_sub_id,
                  sink = std::move(sink)](oai::sba::response r) mutable {
        cont_monitoring_event_unsubscribe(
            scs_as_id, sub_id, nf_sub_id, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_monitoring_event_unsubscribe(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& nf_sub_id, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_monitoring_event_unsubscribe sub_id=%s status=%d", sub_id.c_str(),
      r.status_code);
  // Best-effort: AMF result ignored
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
