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

// Analytics /fetch endpoint
//------------------------------------------------------------------------------
void nef_app::handle_analytics_fetch(
    const std::string& scs_as_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          scs_as_id, NEF_SERVICE_ANALYTICS, response_body, http_code)) {
    return;
  }
  if (!body.contains("analyEventsSubs")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
        "Missing analyEventsSubs in request body");
    return;
  }
  Logger::nef_app().info(
      "Analytics fetch requested by %s: %s", scs_as_id.c_str(),
      body.dump().c_str());

  // Answer from local state only, with no southbound call: collect the
  // stored data of every ANALYTICS subscription owned by this AF whose
  // analyEventsSubs overlap the requested events.
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
      // Keep this subscription only if one of its stored event types is
      // among the requested ones.
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

// Analytics Subscription (TS 29.122) API handlers

//------------------------------------------------------------------------------
// TS 29.522 AnalyticsEvent -> TS 29.520 NwdafEvent. Empty for an event the
// NWDAF has no counterpart for.
static std::string analytics_event_to_nwdaf_event(const std::string& ev) {
  if (ev == "UE_COMM") return "UE_COMMUNICATION";
  if (ev == "ABNORMAL_BEHAVIOR") return "ABNORMAL_BEHAVIOUR";
  if (ev == "CONGESTION") return "USER_DATA_CONGESTION";
  for (const char* same :
       {"UE_MOBILITY", "NETWORK_PERFORMANCE", "QOS_SUSTAINABILITY",
        "DISPERSION", "DN_PERFORMANCE", "SERVICE_EXPERIENCE"}) {
    if (ev == same) return ev;
  }
  return "";
}

//------------------------------------------------------------------------------
// TS 29.522 AnalyticsExposureSubsc -> TS 29.520 NnwdafEventsSubscription.
//
// Each analyEventsSubs[] entry becomes one eventSubscriptions[] entry, its
// analyEventFilter fields carried under their 29.520 names, and analyRepInfo
// (ReportingInformation, TS 29.523) becomes each entry's notificationMethod
// and repetitionPeriod: ON_EVENT_DETECTION -> THRESHOLD, anything else ->
// PERIODIC.
//
// tgtUe: anyUeInd -> anyUe. A GPSI should be translated to a SUPI through the
// UDM (TS 23.288 §6.1.2.2); OAI NEF does not do that translation yet, so a
// GPSI given in SUPI form ("imsi-...") is passed through as the SUPI and any
// other GPSI is forwarded as a gpsi.
static bool to_nwdaf_events_subscription(
    const nlohmann::json& af, const std::string& nef_notif_uri,
    nlohmann::json& out, std::string& err) {
  std::string method = "PERIODIC";
  int period         = 10;
  if (af.contains("analyRepInfo") && af["analyRepInfo"].is_object()) {
    const auto& rep = af["analyRepInfo"];
    if (rep.value("notifMethod", "") == "ON_EVENT_DETECTION")
      method = "THRESHOLD";
    if (rep.contains("repPeriod") && rep["repPeriod"].is_number_integer() &&
        rep["repPeriod"].get<int>() > 0)
      period = rep["repPeriod"].get<int>();
  }

  // Filter fields whose name is the same in both specs.
  static const char* kSame[] = {
      "matchingDir",  "ranUeThrouThds", "qosFlowRetThds", "excepRequs",
      "appIds",       "dnais",          "exptAnaType",    "exptUeBehav",
      "reptThlds",    "nsiIdInfos",     "disperReqs",     "listOfAnaSubsets",
      "dnPerfReqs",   "bwRequs",        "ratFreqs",       "appServerAddrs",
      "extraReportReq", "maxNumOfTopAppUl", "maxNumOfTopAppDl",
      "visitedLocAreas"};

  nlohmann::json subs = nlohmann::json::array();
  for (const auto& ae : af["analyEventsSubs"]) {
    const std::string ev = ae.value("analyEvent", "");
    const std::string nwdaf_ev = analytics_event_to_nwdaf_event(ev);
    if (nwdaf_ev.empty()) {
      err = "analyEvent '" + ev + "' is not supported";
      return false;
    }
    nlohmann::json es         = nlohmann::json::object();
    es["event"]               = nwdaf_ev;
    es["notificationMethod"]  = method;
    es["repetitionPeriod"]    = period;
    if (ae.contains("analyEventFilter") && ae["analyEventFilter"].is_object()) {
      const auto& f = ae["analyEventFilter"];
      for (const char* k : kSame)
        if (f.contains(k)) es[k] = f[k];
      if (f.contains("nwPerfReqs")) es["nwPerfRequs"] = f["nwPerfReqs"];
      if (f.contains("qosReq")) es["qosRequ"] = f["qosReq"];
      if (f.contains("snssai")) es["snssaia"] = nlohmann::json::array({f["snssai"]});
      if (f.contains("dnn")) es["dnns"] = nlohmann::json::array({f["dnn"]});
      if (f.contains("locArea") && f["locArea"].contains("nwAreaInfo"))
        es["networkArea"] = f["locArea"]["nwAreaInfo"];
    }
    if (ae.contains("tgtUe") && ae["tgtUe"].is_object()) {
      const auto& t = ae["tgtUe"];
      nlohmann::json tgt = nlohmann::json::object();
      if (t.value("anyUeInd", false)) tgt["anyUe"] = true;
      if (t.contains("gpsi") && t["gpsi"].is_string()) {
        const std::string g = t["gpsi"];
        if (g.rfind("imsi-", 0) == 0)
          tgt["supis"] = nlohmann::json::array({g});
        else
          tgt["gpsis"] = nlohmann::json::array({g});
      }
      if (!tgt.empty()) es["tgtUe"] = tgt;
    }
    subs.push_back(es);
  }
  out                       = nlohmann::json::object();
  out["notificationURI"]    = nef_notif_uri;
  out["eventSubscriptions"] = subs;
  return true;
}
//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_create(
    const std::string& af_id, const nlohmann::json& body,
    std::string& analytics_sub_id, nlohmann::json& response_body,
    int& http_code) {
  Logger::nef_app().info(
      "Analytics subscription create for AF: %s", af_id.c_str());

  if (reject_unauthorized_af(
          af_id, NEF_SERVICE_ANALYTICS, response_body, http_code)) {
    return;
  }

  if (!body.contains("analyEventsSubs") || !body.contains("notifUri") ||
      !body.contains("notifId")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
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
      http_code = http_status_code::UNPROCESSABLE_ENTITY;
      response_body =
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, err);
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
          http_status_code::BAD_REQUEST, "notifUri: " + uri_err);
      return;
    }
  }

  generate_af_subscription_id(analytics_sub_id);

  // Relay to the NWDAF (Nnwdaf_EventsSubscription). Its notifications come
  // back to NEF's own notify endpoint, keyed by this AF subscription id.
  nlohmann::json nwdaf_body;
  std::string translate_err;
  if (!to_nwdaf_events_subscription(
          body, m_nef_client->get_nef_notify_uri(analytics_sub_id), nwdaf_body,
          translate_err)) {
    http_code = http_status_code::BAD_REQUEST;
    response_body =
        make_problem_detail(http_status_code::BAD_REQUEST, translate_err);
    return;
  }
  std::string nwdaf_sub_id;
  uint32_t nwdaf_code = 0;
  if (!m_nef_client->create_nwdaf_events_subscription(
          nwdaf_body, nwdaf_sub_id, nwdaf_code)) {
    http_code     = http_status_code::INTERNAL_SERVER_ERROR;
    response_body = make_problem_detail(
        http_status_code::INTERNAL_SERVER_ERROR,
        "Failed to create analytics subscription in NWDAF");
    return;
  }

  auto sub = std::make_shared<nef_subscription>(m_event_sub);
  sub->set_af_subscription_id(analytics_sub_id);
  sub->set_nf_subscription_id(nwdaf_sub_id);
  sub->set_scs_as_id(af_id);
  sub->set_service_type(nef_service_type_t::NEF_SERVICE_TYPE_ANALYTICS);
  sub->set_target_nf_type(nf_type_t::NF_TYPE_NWDAF);
  sub->set_notification_uri(body["notifUri"].get<std::string>());
  sub->set_subscription_data(body);

  add_subscription(analytics_sub_id, sub);
  ensure_af_profile(af_id, analytics_sub_id);
  {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id[analytics_sub_id] = analytics_sub_id;
  }
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

  // Best-effort: a failed NWDAF unsubscribe is logged, local state still goes.
  const std::string nwdaf_sub_id = sub->get_nf_subscription_id();
  if (!nwdaf_sub_id.empty())
    m_nef_client->delete_nwdaf_events_subscription(nwdaf_sub_id);
  {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    m_nf2af_sub_id.erase(analytics_sub_id);
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
  if (reject_unauthorized_af(
          af_id, NEF_SERVICE_ANALYTICS, response_body, http_code)) {
    return;
  }

  auto sub = find_subscription(analytics_sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Analytics subscription not found");
    return;
  }

  if (!is_subscription_owner(sub, af_id)) {
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
void nef_app::handle_analytics_subscription_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          af_id, NEF_SERVICE_ANALYTICS, response_body, http_code)) {
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

// Analytics update (PUT)
//------------------------------------------------------------------------------
void nef_app::handle_analytics_subscription_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          scs_as_id, NEF_SERVICE_ANALYTICS, response_body, http_code)) {
    return;
  }
  auto sub = find_subscription(sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "Analytics subscription not found");
    return;
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN,
        "AF is not allowed to access this subscription");
    return;
  }
  if (!body.contains("analyEventsSubs") || !body.contains("notifUri") ||
      !body.contains("notifId")) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST,
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
      http_code = http_status_code::UNPROCESSABLE_ENTITY;
      response_body =
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, err);
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
          http_status_code::BAD_REQUEST, "notifUri: " + uri_err);
      return;
    }
  }
  // Update the NWDAF first, so a rejected change leaves both sides as they
  // were and the AF is told.
  {
    nlohmann::json nwdaf_body;
    std::string translate_err;
    if (!to_nwdaf_events_subscription(
            body, m_nef_client->get_nef_notify_uri(sub_id), nwdaf_body,
            translate_err)) {
      http_code = http_status_code::BAD_REQUEST;
      response_body =
          make_problem_detail(http_status_code::BAD_REQUEST, translate_err);
      return;
    }
    uint32_t nwdaf_code = 0;
    const std::string nwdaf_sub_id = sub->get_nf_subscription_id();
    if (!nwdaf_sub_id.empty() &&
        !m_nef_client->update_nwdaf_events_subscription(
            nwdaf_sub_id, nwdaf_body, nwdaf_code)) {
      http_code     = http_status_code::INTERNAL_SERVER_ERROR;
      response_body = make_problem_detail(
          http_status_code::INTERNAL_SERVER_ERROR,
          "Failed to update analytics subscription in NWDAF");
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
