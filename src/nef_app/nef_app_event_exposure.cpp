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
static std::string build_nnef_event_exposure_subscription_path(
    const std::string& subscription_id) {
  return "/nnef-eventexposure/v1/subscriptions/" + subscription_id;
}

// Nnef_EventExposure (TS 29.591)
//------------------------------------------------------------------------------
void nef_app::handle_nnef_event_exposure_subscribe(
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_nf(
          NEF_SERVICE_MONITORING_EVENT, response_body, http_code)) {
    return;
  }

  std::string error_detail;

  // Typed parse + validate; this replaced the hand-written
  // validate_nnef_event_exposure_subscription_body().
  oai::_3gpp::model::NefEventExposureSubsc subsc;
  try {
    from_json(body, subsc);
    subsc.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY,
        std::string("Validation failed: ") + e.what());
    return;
  } catch (const std::exception& e) {
    // e.g. std::invalid_argument thrown by a generated enum from_json on an
    // unrecognised value. Treat as a malformed request body (400) rather than
    // letting it escape and abort the process.
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        std::string("Invalid body: ") + e.what());
    return;
  }

  // TS 29.591 §5.4.2: each NefEventSubs must have a valid event
  const auto& events_subs = subsc.getEventsSubs();
  if (events_subs.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        "eventsSubs is required and must be a non-empty array");
    return;
  }
  for (std::size_t i = 0; i < events_subs.size(); ++i) {
    if (events_subs[i].getEvent().getEnumValue() ==
        oai::_3gpp::model::NefEvent_anyOf::eNefEvent_anyOf::
            INVALID_VALUE_OPENAPI_GENERATED) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST,
          "eventsSubs[" + std::to_string(i) +
              "].event: required non-empty string");
      return;
    }
  }

  if (subsc.getNotifUri().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        "notifUri is required and must be a non-empty string");
    return;
  }
  if (subsc.getNotifId().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        "notifId is required and must be a non-empty string");
    return;
  }

  error_detail = validate_callback_uri(subsc.getNotifUri());
  if (!error_detail.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "notifUri: " + error_detail);
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
  if (reject_unauthorized_nf(
          NEF_SERVICE_MONITORING_EVENT, response_body, http_code)) {
    return;
  }

  std::shared_lock lock(m_nnef_event_subscriptions_mutex);
  auto it = m_nnef_event_subscriptions.find(subscription_id);
  if (it == m_nnef_event_subscriptions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND,
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
  if (reject_unauthorized_nf(
          NEF_SERVICE_MONITORING_EVENT, response_body, http_code)) {
    return;
  }

  std::string error_detail;

  // Typed parse + validate, same shape as the subscribe handler.
  oai::_3gpp::model::NefEventExposureSubsc subsc;
  try {
    from_json(body, subsc);
    subsc.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        std::string("Invalid body: ") + e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code     = http_status_code::UNPROCESSABLE_ENTITY;
    response_body = make_problem_detail(
        http_status_code::UNPROCESSABLE_ENTITY,
        std::string("Validation failed: ") + e.what());
    return;
  } catch (const std::exception& e) {
    // e.g. std::invalid_argument thrown by a generated enum from_json on an
    // unrecognised value. Treat as a malformed request body (400) rather than
    // letting it escape and abort the process.
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        std::string("Invalid body: ") + e.what());
    return;
  }

  const auto& events_subs = subsc.getEventsSubs();
  if (events_subs.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        "eventsSubs is required and must be a non-empty array");
    return;
  }
  for (std::size_t i = 0; i < events_subs.size(); ++i) {
    if (events_subs[i].getEvent().getEnumValue() ==
        oai::_3gpp::model::NefEvent_anyOf::eNefEvent_anyOf::
            INVALID_VALUE_OPENAPI_GENERATED) {
      http_code     = http_status_code::BAD_REQUEST;
      response_body = make_problem_detail(
          http_status_code::BAD_REQUEST,
          "eventsSubs[" + std::to_string(i) +
              "].event: required non-empty string");
      return;
    }
  }

  if (subsc.getNotifUri().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        "notifUri is required and must be a non-empty string");
    return;
  }
  if (subsc.getNotifId().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        "notifId is required and must be a non-empty string");
    return;
  }

  error_detail = validate_callback_uri(subsc.getNotifUri());
  if (!error_detail.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "notifUri: " + error_detail);
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_event_subscriptions_mutex);
    auto it = m_nnef_event_subscriptions.find(subscription_id);
    if (it == m_nnef_event_subscriptions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND,
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
