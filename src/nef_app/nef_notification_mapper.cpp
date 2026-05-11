/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_notification_mapper.hpp"
#include "logger.hpp"

using namespace oai::nef::app;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Get a string field from an Object, returning `def` if absent or non-string.
static std::string get_str(
    const rfl::Generic::Object& obj, const std::string& key,
    const std::string& def = "") {
  auto r = obj.get(key);
  if (!r) return def;
  if (auto* s = std::get_if<std::string>(&r.value().variant())) return *s;
  return def;
}

// Copy a field from src to dst if it exists in src.
static void copy_field(
    const rfl::Generic::Object& src, rfl::Generic::Object& dst,
    const std::string& key) {
  auto r = src.get(key);
  if (r) dst[key] = r.value();
}

// RFC 7807 problem-detail as rfl::Generic
static rfl::Generic make_problem(
    const std::string& title, const std::string& detail) {
  rfl::Generic::Object obj;
  obj["title"]  = rfl::Generic(title);
  obj["detail"] = rfl::Generic(detail);
  obj["status"] = rfl::Generic(int64_t(0));
  return rfl::Generic(std::move(obj));
}

}  // namespace

// AMF → T8 Monitoring Event Notification
//------------------------------------------------------------------------------
bool nef_notification_mapper::amf_to_monitoring_notification(
    const rfl::Generic& amf_notif, rfl::Generic& t8_notif,
    const std::string& sub_id) {
  // AMF EventExposure Notify schema (TS 29.518 §6.3.4.3.3):
  //   { "subscriptionId": "...",
  //     "reportList": [ { "type": "LOCATION_REPORT",
  //                        "state": { ... },
  //                        "supi": "...",
  //                        "timeStamp": "..." } ] }
  //
  // T8 MonitoringEventNotification (TS 29.122 §8.4.4.3.2):
  //   { "subscription": "<sub_id>",
  //     "monitoringEventReports": [
  //       { "monitoringType": "LOCATION_REPORTING",
  //         "locationInfo": { ... },
  //         "supi": "...",
  //         "timeStamp": "..." } ] }

  rfl::Generic::Object out;
  out["subscription"] = rfl::Generic(sub_id);

  const auto* amf_obj = std::get_if<rfl::Generic::Object>(&amf_notif.variant());
  if (!amf_obj) {
    Logger::nef_app().warn(
        "amf_to_monitoring_notification: input is not a JSON object");
    t8_notif = make_problem("Bad AMF notification", "not a JSON object");
    return false;
  }

  auto rl_r = amf_obj->get("reportList");
  if (!rl_r) {
    Logger::nef_app().warn(
        "amf_to_monitoring_notification: missing/invalid 'reportList'");
    t8_notif = make_problem("Bad AMF notification", "reportList not found");
    return false;
  }
  const auto* arr_ptr =
      std::get_if<rfl::Generic::Array>(&rl_r.value().variant());
  if (!arr_ptr) {
    Logger::nef_app().warn(
        "amf_to_monitoring_notification: 'reportList' is not an array");
    t8_notif = make_problem("Bad AMF notification", "reportList not an array");
    return false;
  }

  rfl::Generic::Array reports;
  for (const auto& r_elem : *arr_ptr) {
    const auto* r = std::get_if<rfl::Generic::Object>(&r_elem.variant());
    if (!r) continue;

    rfl::Generic::Object t8_report;

    // Translate AMF event type to T8 monitoringType
    std::string amf_type = get_str(*r, "type", "UNKNOWN");
    if (amf_type == "LOCATION_REPORT") {
      t8_report["monitoringType"] =
          rfl::Generic(std::string("LOCATION_REPORTING"));
      auto state_r = r->get("state");
      if (state_r) t8_report["locationInfo"] = state_r.value();
    } else if (amf_type == "UE_REACHABILITY_FOR_SMS") {
      t8_report["monitoringType"] =
          rfl::Generic(std::string("UE_REACHABILITY"));
      t8_report["reachabilityForSms"] = rfl::Generic(true);
    } else if (amf_type == "UE_REACHABILITY_FOR_DATA") {
      t8_report["monitoringType"] =
          rfl::Generic(std::string("UE_REACHABILITY"));
      t8_report["reachabilityForData"] = rfl::Generic(true);
    } else if (amf_type == "PDU_SESSION_STATUS") {
      t8_report["monitoringType"] =
          rfl::Generic(std::string("PDU_SESSION_STATUS"));
      auto pdu_r = r->get("pduSessionStatusList");
      if (pdu_r) t8_report["pduSessionStatus"] = pdu_r.value();
    } else {
      // Pass-through for unknown types
      t8_report["monitoringType"] = rfl::Generic(amf_type);
      auto state_r                = r->get("state");
      if (state_r) t8_report["stateInfo"] = state_r.value();
    }

    // Common fields
    for (const char* field : {"supi", "timeStamp", "gpsi"}) {
      copy_field(*r, t8_report, field);
    }

    reports.push_back(rfl::Generic(std::move(t8_report)));
  }
  out["monitoringEventReports"] = rfl::Generic(std::move(reports));
  t8_notif                      = rfl::Generic(std::move(out));
  return true;
}

// SMF → T8 Session-with-QoS Notification
//------------------------------------------------------------------------------
bool nef_notification_mapper::smf_to_qos_notification(
    const rfl::Generic& smf_notif, rfl::Generic& t8_notif,
    const std::string& sub_id) {
  // SMF EventExposure Notify (TS 29.508 §4.6.3):
  //   { "subscriptionId": "...",
  //     "notifId": "...",
  //     "eventNotifs": [ { "event": "QOS_MONITORING",
  //                         "qosMonitoringMeasurement": { ... },
  //                         "supi": "...",
  //                         "timeStamp": "..." } ] }
  //
  // T8 AsSessionWithQoSEventNotification (TS 29.122 §8.6.4.3.2):
  //   { "subscription": "<sub_id>",
  //     "evNotifs": [ { "event": "QOS_GUARANTEED",
  //                      "qosMonInfo": { ... },
  //                      "supi": "...",
  //                      "timeStamp": "..." } ] }

  rfl::Generic::Object out;
  out["subscription"] = rfl::Generic(sub_id);

  const auto* smf_obj = std::get_if<rfl::Generic::Object>(&smf_notif.variant());
  if (!smf_obj) {
    Logger::nef_app().warn(
        "smf_to_qos_notification: input is not a JSON object");
    t8_notif = make_problem("Bad SMF notification", "not a JSON object");
    return false;
  }

  auto ev_r = smf_obj->get("eventNotifs");
  if (!ev_r) {
    Logger::nef_app().warn(
        "smf_to_qos_notification: missing/invalid 'eventNotifs'");
    t8_notif = make_problem("Bad SMF notification", "eventNotifs not found");
    return false;
  }
  const auto* arr_ptr =
      std::get_if<rfl::Generic::Array>(&ev_r.value().variant());
  if (!arr_ptr) {
    Logger::nef_app().warn(
        "smf_to_qos_notification: 'eventNotifs' is not an array");
    t8_notif = make_problem("Bad SMF notification", "eventNotifs not an array");
    return false;
  }

  rfl::Generic::Array ev_notifs;
  for (const auto& e_elem : *arr_ptr) {
    const auto* e = std::get_if<rfl::Generic::Object>(&e_elem.variant());
    if (!e) continue;

    rfl::Generic::Object t8_ev;

    std::string smf_event = get_str(*e, "event", "UNKNOWN");
    if (smf_event == "QOS_MONITORING") {
      t8_ev["event"] = rfl::Generic(std::string("QOS_GUARANTEED"));
      auto qos_r     = e->get("qosMonitoringMeasurement");
      if (qos_r) t8_ev["qosMonInfo"] = qos_r.value();
    } else if (smf_event == "PDU_SESSION_RELEASE") {
      t8_ev["event"] = rfl::Generic(std::string("SESSION_TERMINATION"));
    } else {
      t8_ev["event"] = rfl::Generic(smf_event);
    }

    for (const char* field : {"supi", "timeStamp", "pduSeId"}) {
      copy_field(*e, t8_ev, field);
    }

    ev_notifs.push_back(rfl::Generic(std::move(t8_ev)));
  }
  out["evNotifs"] = rfl::Generic(std::move(ev_notifs));
  t8_notif        = rfl::Generic(std::move(out));
  return true;
}

// PCF → T8 Traffic Influence Notification
//------------------------------------------------------------------------------
bool nef_notification_mapper::pcf_to_ti_notification(
    const rfl::Generic& pcf_notif, rfl::Generic& t8_notif,
    const std::string& sub_id) {
  // PCF PolicyAuthorization Notify (TS 29.514 §4.2.3.4):
  //   { "appSessionId": "...",
  //     "evNotifs": [ { "event": "USAGE_REPORT",
  //                      "usgRep": { ... },
  //                      "timeStamp": "..." } ] }
  //
  // T8 TrafficInfluenceNotification (TS 29.522 §8.3.2):
  //   { "subscription": "<sub_id>",
  //     "trafficInfluenceNotifs": [ { "dnaiChgType": "...",
  //                                   "targetDnai": "...",
  //                                   "timeStamp": "..." } ] }

  rfl::Generic::Object out;
  out["subscription"] = rfl::Generic(sub_id);

  const auto* pcf_obj = std::get_if<rfl::Generic::Object>(&pcf_notif.variant());
  if (!pcf_obj) {
    Logger::nef_app().warn(
        "pcf_to_ti_notification: input is not a JSON object");
    t8_notif = make_problem("Bad PCF notification", "not a JSON object");
    return false;
  }

  auto ev_r = pcf_obj->get("evNotifs");
  if (!ev_r) {
    Logger::nef_app().warn(
        "pcf_to_ti_notification: missing/invalid 'evNotifs'");
    t8_notif = make_problem("Bad PCF notification", "evNotifs not found");
    return false;
  }
  const auto* arr_ptr =
      std::get_if<rfl::Generic::Array>(&ev_r.value().variant());
  if (!arr_ptr) {
    Logger::nef_app().warn(
        "pcf_to_ti_notification: 'evNotifs' is not an array");
    t8_notif = make_problem("Bad PCF notification", "evNotifs not an array");
    return false;
  }

  rfl::Generic::Array ti_notifs;
  for (const auto& e_elem : *arr_ptr) {
    const auto* e = std::get_if<rfl::Generic::Object>(&e_elem.variant());
    if (!e) continue;

    rfl::Generic::Object ti_ev;

    std::string pcf_event = get_str(*e, "event", "UNKNOWN");
    if (pcf_event == "DNAI_CH_REPORT") {
      ti_ev["dnaiChgType"] = rfl::Generic(get_str(*e, "dnaiChgType", "EARLY"));
      copy_field(*e, ti_ev, "sourceDnai");
      copy_field(*e, ti_ev, "targetDnai");
    } else if (pcf_event == "USAGE_REPORT") {
      auto usg_r = e->get("usgRep");
      if (usg_r) {
        ti_ev["usageReport"] = usg_r.value();
      } else {
        ti_ev["usageReport"] = rfl::Generic(rfl::Generic::Object{});
      }
    } else {
      ti_ev["event"] = rfl::Generic(pcf_event);
    }

    copy_field(*e, ti_ev, "timeStamp");
    ti_notifs.push_back(rfl::Generic(std::move(ti_ev)));
  }
  out["trafficInfluenceNotifs"] = rfl::Generic(std::move(ti_notifs));
  t8_notif                      = rfl::Generic(std::move(out));
  return true;
}
