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

  // NEF inbound endpoint (not the AF notificationDestination)
  // PCF callback template is "{evSubsc/notifUri}/notify"; set both fields.
  if (!evsubsc_notif_uri.empty()) asc["notifUri"] = evsubsc_notif_uri;

  // Media components from flowInfo[]/qosReference
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

  // Event subscription block
  // Only when an inbound NEF endpoint is supplied (CREATE/PUT, not PATCH).
  if (!evsubsc_notif_uri.empty()) {
    nlohmann::json ev_subsc = nlohmann::json::object();
    ev_subsc["notifUri"]    = evsubsc_notif_uri;

    // Translate requested T8 UserPlaneEvent -> PCF AfEvent, de-duplicating.
    // SUCCESSFUL_/FAILED_RESOURCES_ALLOCATION are always present
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
void nef_app::handle_qos_subscription_get(
    const std::string& af_id, const std::string& qos_sub_id,
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          af_id, NEF_SERVICE_QOS_MONITORING, response_body, http_code)) {
    return;
  }

  auto sub = find_subscription(qos_sub_id);
  if (!sub) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "QoS subscription not found");
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
void nef_app::handle_qos_subscription_list(
    const std::string& af_id, nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          af_id, NEF_SERVICE_QOS_MONITORING, response_body, http_code)) {
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

//------------------------------------------------------------------------------
// qos_subscription_create. The pre-southbound work is unchanged; only the PCF
// policy-auth create becomes an async fire. cont_qos_create now owns the
// failure branch, the id wiring, and the success body with its relative `self`
// URI. Rewriting that `self` into an absolute URI stays in the adapter header
// sink.
//
// Parity with the sync handler: cont_qos_create resolves the PCF appSessionId
// from the raw response — JSON appSessionId first, else the Location header,
// the same precedence as the sync create_pcf_policy_auth. It then applies the
// same is_valid_app_session_id() guard the sync handler applies. On a
// southbound timeout, status 0, 4xx or 5xx the parsed id comes back empty or
// invalid, which is FATAL-500.
void nef_app::qos_create(
    const std::string& af_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info("QoS subscription create for AF: %s", af_id.c_str());

  if (reject_unauthorized_af(af_id, NEF_SERVICE_QOS_MONITORING, sink)) return;

  oai::_3gpp::model::AsSessionWithQoSSubscription req_data = {};
  try {
    from_json(body, req_data);
    req_data.validate();
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

  if (req_data.getNotificationDestination().empty()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
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
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, err)
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
              http_status_code::BAD_REQUEST,
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
        make_problem_detail(http_status_code::BAD_REQUEST, pcf_translate_err)
            .dump());
  }

  // Capture the typed request as JSON so the continuation can rebuild the
  // success body (set self URI) without re-parsing the AF body.
  nlohmann::json req_data_json;
  to_json(req_data_json, req_data);
  clear_request_bearer_token();

  // Fire the PCF policy-auth create.
  m_nef_client->create_pcf_policy_auth_async(
      pcf_body, [this, af_id, qos_sub_id, req_data_json,
                 sink = std::move(sink)](oai::sba::response r) mutable {
        cont_qos_create(
            af_id, qos_sub_id, std::move(req_data_json), std::move(r),
            std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_qos_create(
    const std::string& af_id, const std::string& qos_sub_id,
    nlohmann::json req_data_json, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_qos_create qos_sub_id=%s status=%d", qos_sub_id.c_str(),
      r.status_code);
  const std::string pcf_app_session_id =
      sbi_ok(r) ? nef_async_parse_pcf_app_session_id(r) : "";
  // FATAL-500, not 502: PCF has to both succeed and hand back a usable
  // appSessionId, so a missing or malformed id is as fatal as an error status.
  if (!sbi_ok(r) || !is_valid_app_session_id(pcf_app_session_id)) {
    remove_subscription(qos_sub_id);
    release_af_profile_subscription(af_id, qos_sub_id);
    return sink(
        http_status_code::INTERNAL_SERVER_ERROR,
        make_problem_detail(
            http_status_code::INTERNAL_SERVER_ERROR,
            "Failed to create policy authorization in PCF")
            .dump());
  }

  // A concurrent AF delete may have removed qos_sub_id while PCF was in
  // flight. If it is gone, do not resurrect it: the AF delete already won, so
  // this is a benign no-op that answers 204. The freshly-created PCF
  // app-session is left for PCF/AF cleanup — the sync QoS create has no
  // compensating southbound delete either.
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

//------------------------------------------------------------------------------
// qos_subscription_update (PUT). Authorize, typed-parse, validate, check the
// owner and the immutable fields, resolve the PCF app-session and update the
// local store — all unchanged. Only the PCF policy-auth update becomes an
// async fire.
//
// BEST-EFFORT: the AF gets the stored subscription data whatever PCF says, and
// a PCF failure only warns.
//
// When the PCF body cannot be translated, the sync path fires no southbound
// call at all, so the async path matches it by calling cont_qos_update inline.
void nef_app::qos_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_af(scs_as_id, NEF_SERVICE_QOS_MONITORING, sink)) {
    return;
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

  auto sub = find_subscription(sub_id);
  if (!sub) {
    clear_request_bearer_token();
    return sink(
        http_status_code::NOT_FOUND,
        make_problem_detail(
            http_status_code::NOT_FOUND, "QoS subscription not found")
            .dump());
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN,
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
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, err)
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
              http_status_code::BAD_REQUEST,
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
                http_status_code::BAD_REQUEST,
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
            http_status_code::NOT_FOUND,
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
  // southbound call and still returns 200 — finish inline.
  nlohmann::json pcf_patch;
  std::string pcf_err;
  if (build_pcf_qos_body(
          update_data, /*evsubsc_notif_uri=*/"", pcf_patch, pcf_err)) {
    nlohmann::json merge =
        pcf_patch.value("ascReqData", nlohmann::json::object());
    merge.erase("evSubsc");  // subscription persists per §4.15.6.6a
    // Fire the PCF policy-auth update (best-effort).
    m_nef_client->update_pcf_policy_auth_async(
        app_session_id, merge,
        [this, scs_as_id, sub_id, response_data,
         sink = std::move(sink)](oai::sba::response r) mutable {
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
        scs_as_id, sub_id, std::move(response_data), oai::sba::response{},
        std::move(sink));
  }
}

//------------------------------------------------------------------------------
void nef_app::cont_qos_update(
    const std::string& scs_as_id, const std::string& sub_id,
    nlohmann::json response_data, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_qos_update sub_id=%s status=%d", sub_id.c_str(), r.status_code);
  // Best-effort: PCF result is warn-only.
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "T8 PUT: PCF update failed for sub=%s (http=%d); in-memory state "
        "updated, PCF best-effort",
        sub_id.c_str(), r.status_code);
  }
  nef_audit::log("UPDATE", "QOS", scs_as_id, sub_id, http_status_code::OK);
  sink(http_status_code::OK, response_data.dump());
}

// qos_subscription_patch. Authorize, check the owner, merge-patch, validate,
// SSRF-check the callback URI, check the immutable fields, resolve the PCF
// app-session and update the local store — all unchanged. Only the PCF
// policy-auth update becomes an async fire.
//
// BEST-EFFORT: the AF gets the patched body whatever PCF says, and a PCF
// failure only warns.
//
// When the PCF body cannot be translated, the sync path fires no southbound
// call, so the async path matches it by calling cont_qos_patch inline.
void nef_app::qos_patch(
    const std::string& scs_as_id, const std::string& sub_id,
    const nlohmann::json& patch_body, const std::string& token,
    response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_af(scs_as_id, NEF_SERVICE_QOS_MONITORING, sink)) {
    return;
  }

  auto sub = find_subscription(sub_id);
  if (!sub) {
    clear_request_bearer_token();
    return sink(
        http_status_code::NOT_FOUND,
        make_problem_detail(
            http_status_code::NOT_FOUND, "QoS subscription not found")
            .dump());
  }
  if (!is_subscription_owner(sub, scs_as_id)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::FORBIDDEN,
        make_problem_detail(
            http_status_code::FORBIDDEN,
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
            http_status_code::BAD_REQUEST,
            std::string("Invalid patched body: ") + e.what())
            .dump());
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            std::string("Patch produces an invalid subscription: ") + e.what())
            .dump());
  } catch (const std::exception& e) {
    // e.g. std::invalid_argument thrown by a generated enum from_json on an
    // unrecognised value in the merged/patched body. Treat as a malformed
    // request (400) rather than letting it escape and abort the process.
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
            std::string("Invalid patched body: ") + e.what())
            .dump());
  }
  if (merged_data.getNotificationDestination().empty()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST,
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
              http_status_code::BAD_REQUEST,
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
                http_status_code::BAD_REQUEST,
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
            http_status_code::NOT_FOUND,
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
    // Fire the PCF policy-auth update (best-effort).
    m_nef_client->update_pcf_policy_auth_async(
        app_session_id, merge,
        [this, scs_as_id, sub_id, patched,
         sink = std::move(sink)](oai::sba::response r) mutable {
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
        scs_as_id, sub_id, std::move(patched), oai::sba::response{},
        std::move(sink));
  }
}

//------------------------------------------------------------------------------
void nef_app::cont_qos_patch(
    const std::string& scs_as_id, const std::string& sub_id,
    nlohmann::json patched, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_qos_patch sub_id=%s status=%d", sub_id.c_str(), r.status_code);
  // Best-effort: whatever PCF says, the patch is reported as successful.
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "T8 PATCH: PCF update failed for sub=%s (http=%d); in-memory state "
        "updated",
        sub_id.c_str(), r.status_code);
  }
  nef_audit::log("PATCH", "QOS", scs_as_id, sub_id, http_status_code::OK);
  sink(http_status_code::OK, patched.dump());
}

// qos_subscription_delete. Authorize and check the owner — unchanged. Only the
// SMF event-exposure unsubscribe becomes an async fire, and its result is
// unchecked.
//
// BEST-EFFORT: cont_qos_delete does the local cleanup and answers 204 whatever
// SMF says.
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
        af_id, qos_sub_id, nf_sub_id, oai::sba::response{}, std::move(sink));
  }
  // Fire the SMF unsubscribe; whatever it answers, we return 204.
  m_nef_client->unsubscribe_smf_event_exposure_async(
      nf_sub_id, [this, af_id, qos_sub_id, nf_sub_id,
                  sink = std::move(sink)](oai::sba::response r) mutable {
        cont_qos_delete(
            af_id, qos_sub_id, nf_sub_id, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_qos_delete(
    const std::string& af_id, const std::string& qos_sub_id,
    const std::string& nf_sub_id, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_qos_delete qos_sub_id=%s status=%d", qos_sub_id.c_str(),
      r.status_code);
  // Best-effort: SMF result ignored.
  if (!nf_sub_id.empty() && !sbi_ok(r)) {
    Logger::nef_app().warn(
        "SMF event unsubscribe failed for nf_sub_id=%s (http=%d); local "
        "cleanup proceeds",
        nf_sub_id.c_str(), r.status_code);
  }
  if (!nf_sub_id.empty()) {
    const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
    // cont_qos_create inserts two keys pointing at this af mapping: the
    // notifId (== qos_sub_id) and the PCF appSessionId (== nf_sub_id). Erase
    // both, or the appSessionId entry leaks for the life of the process.
    m_nf2af_sub_id.erase(qos_sub_id);
    m_nf2af_sub_id.erase(nf_sub_id);
  }
  remove_subscription(qos_sub_id);
  release_af_profile_subscription(af_id, qos_sub_id);
  nef_audit::log(
      "DELETE", "QOS", af_id, qos_sub_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}
