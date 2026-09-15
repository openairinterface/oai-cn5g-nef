/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_notification_mapper.hpp"
#include "logger.hpp"

using namespace oai::nef::app;
using json = nlohmann::json;

// Builds the RFC 7807 problem-detail returned on a mapping failure.
//------------------------------------------------------------------------------
static json make_problem(const std::string& title, const std::string& detail) {
  return json{{"title", title}, {"detail", detail}, {"status", 0}};
}

// AMF → T8 Monitoring Event Notification
//------------------------------------------------------------------------------
bool nef_notification_mapper::amf_to_monitoring_notification(
    const json& amf_notif, json& t8_notif, const std::string& sub_id) {
  // Inbound: AMF EventExposure Notify (TS 29.518 §6.3.4.3.3):
  //   { "subscriptionId": "...",
  //     "reportList": [ { "type": "LOCATION_REPORT",
  //                        "state": { ... },
  //                        "supi": "...",
  //                        "timeStamp": "..." } ] }
  //
  // Outbound: T8 MonitoringEventNotification (TS 29.122 §8.4.4.3.2):
  //   { "subscription": "<sub_id>",
  //     "monitoringEventReports": [
  //       { "monitoringType": "LOCATION_REPORTING",
  //         "locationInfo": { ... },
  //         "supi": "...",
  //         "timeStamp": "..." } ] }

  t8_notif["subscription"] = sub_id;

  if (!amf_notif.contains("reportList") ||
      !amf_notif["reportList"].is_array()) {
    Logger::nef_app().warn(
        "amf_to_monitoring_notification: missing/invalid 'reportList'");
    t8_notif = make_problem("Bad AMF notification", "reportList not found");
    return false;
  }

  json reports = json::array();
  for (const auto& r : amf_notif["reportList"]) {
    json t8_report;

    // AMF event type -> T8 monitoringType.
    std::string amf_type = r.value("type", "UNKNOWN");
    if (amf_type == "LOCATION_REPORT") {
      t8_report["monitoringType"] = "LOCATION_REPORTING";
      if (r.contains("state")) t8_report["locationInfo"] = r["state"];
    } else if (amf_type == "UE_REACHABILITY_FOR_SMS") {
      t8_report["monitoringType"]     = "UE_REACHABILITY";
      t8_report["reachabilityForSms"] = true;
    } else if (amf_type == "UE_REACHABILITY_FOR_DATA") {
      t8_report["monitoringType"]      = "UE_REACHABILITY";
      t8_report["reachabilityForData"] = true;
    } else if (amf_type == "PDU_SESSION_STATUS") {
      t8_report["monitoringType"] = "PDU_SESSION_STATUS";
      if (r.contains("pduSessionStatusList"))
        t8_report["pduSessionStatus"] = r["pduSessionStatusList"];
    } else {
      // Unknown type: keep the AMF name as-is (forward-compat) and park any
      // state under stateInfo rather than guessing a T8 field for it.
      t8_report["monitoringType"] = amf_type;
      if (r.contains("state")) t8_report["stateInfo"] = r["state"];
    }

    // Fields that ride along on every report type.
    if (r.contains("supi")) t8_report["supi"] = r["supi"];
    if (r.contains("timeStamp")) t8_report["timeStamp"] = r["timeStamp"];
    if (r.contains("gpsi")) t8_report["gpsi"] = r["gpsi"];

    reports.push_back(t8_report);
  }
  t8_notif["monitoringEventReports"] = reports;
  return true;
}

// SMF SmfEvent → T8 UserPlaneEvent mapping (one UserPlaneEventReport)
//------------------------------------------------------------------------------
// Maps one inbound SMF EventNotification entry (TS 29.508) to one T8
// UserPlaneEventReport (TS 29.122).
//
// The whole eventNotif body is needed, not just the event name: QOS_MON splits
// into QOS_GUARANTEED / QOS_NOT_GUARANTEED / QOS_MONITORING according to the
// QoS-Notification-Control type carried inside the body.
//
// The two enums do not line up, and bridging them is the whole job here:
//
//   SmfEvent (TS 29.508): AC_TY_CH, UP_PATH_CH, PDU_SES_REL, PLMN_CH,
//   UE_IP_CH, RAT_TY_CH, DDDS, COMM_FAIL, PDU_SES_EST, QFI_ALLOC, QOS_MON,
//   SMCC_EXP, ... — note there is no QOS_GUARANTEED / QOS_NOT_GUARANTEED SMF
//   event.
//
//   UserPlaneEvent (TS 29.122): SESSION_TERMINATION, LOSS_OF_BEARER,
//   RECOVERY_OF_BEARER, RELEASE_OF_BEARER, USAGE_REPORT,
//   FAILED_RESOURCES_ALLOCATION, QOS_GUARANTEED, QOS_NOT_GUARANTEED,
//   QOS_MONITORING, SUCCESSFUL_RESOURCES_ALLOCATION, ACCESS_TYPE_CHANGE,
//   PLMN_CHG.
//
// The UserPlaneEventReport is built by hand rather than through the model
// classes, so the mapper keeps no link dependency on them. The model classes
// stay the schema / validate() oracle in the tests.
static json map_smf_event_to_userplane(
    const std::string& smf_event, const json& event_notif) {
  json report;

  // Folds the SMF delay measurements into a single QosMonitoringReport entry.
  // A QOS_MON EventNotification carries the delay arrays (ulDelays, dlDelays,
  // rtDelays) plus a packet-delay-measurement-failure flag (pdmf); all four
  // map across one for one. Returns an empty array when none are present.
  auto build_qos_mon_reports = [&event_notif]() -> json {
    json qmr = json::object();
    bool any = false;
    if (event_notif.contains("ulDelays")) {
      qmr["ulDelays"] = event_notif["ulDelays"];
      any             = true;
    }
    if (event_notif.contains("dlDelays")) {
      qmr["dlDelays"] = event_notif["dlDelays"];
      any             = true;
    }
    if (event_notif.contains("rtDelays")) {
      qmr["rtDelays"] = event_notif["rtDelays"];
      any             = true;
    }
    if (event_notif.contains("pdmf")) {
      qmr["pdmf"] = event_notif["pdmf"];
      any         = true;
    }
    json arr = json::array();
    if (any) arr.push_back(qmr);
    return arr;
  };

  if (smf_event == "QOS_MON") {
    // Resolve the QoS-Notification-Control type.
    //
    // Spec deviation: TS 29.508 gives the SMF EventNotification delay
    // measurements only. The GUARANTEED / NOT_GUARANTEED indication
    // (QosNotifType, TS 29.514) is not a standardized top-level field there,
    // so probe the placements seen in practice, in order:
    //   - qosNotifType                          (flat, OAI-SMF specific)
    //   - notifType                             (flat alias)
    //   - qosNotificationControlInfo.notifType  (nested, OAI-SMF specific)
    //
    // None of them present means a pure measurement report -> QOS_MONITORING.
    std::string qos_notif_type;
    if (event_notif.contains("qosNotifType") &&
        event_notif["qosNotifType"].is_string()) {
      qos_notif_type = event_notif["qosNotifType"].get<std::string>();
    } else if (
        event_notif.contains("notifType") &&
        event_notif["notifType"].is_string()) {
      qos_notif_type = event_notif["notifType"].get<std::string>();
    } else if (
        event_notif.contains("qosNotificationControlInfo") &&
        event_notif["qosNotificationControlInfo"].is_object() &&
        event_notif["qosNotificationControlInfo"].contains("notifType") &&
        event_notif["qosNotificationControlInfo"]["notifType"].is_string()) {
      qos_notif_type = event_notif["qosNotificationControlInfo"]["notifType"]
                           .get<std::string>();
    }

    if (qos_notif_type == "GUARANTEED") {
      report["event"] = "QOS_GUARANTEED";
    } else if (qos_notif_type == "NOT_GUARANTEED") {
      report["event"] = "QOS_NOT_GUARANTEED";
    } else {
      report["event"] = "QOS_MONITORING";
    }

    json qmr = build_qos_mon_reports();
    if (!qmr.empty()) report["qosMonReports"] = qmr;
    if (event_notif.contains("appliedQosRef"))
      report["appliedQosRef"] = event_notif["appliedQosRef"];
  } else if (smf_event == "PDU_SES_REL") {
    report["event"] = "SESSION_TERMINATION";
  } else if (smf_event == "AC_TY_CH") {
    report["event"] = "ACCESS_TYPE_CHANGE";
  } else if (smf_event == "PLMN_CH") {
    report["event"] = "PLMN_CHG";
    if (event_notif.contains("plmnId"))
      report["plmnId"] = event_notif["plmnId"];
  } else if (smf_event == "UP_STATUS_INFO") {
    // Best effort: the SMF UP-status signal has no exact T8 analogue. Map it
    // to LOSS_OF_BEARER and forward any usage/flow context that came with it.
    report["event"] = "LOSS_OF_BEARER";
    if (event_notif.contains("accumulatedUsage"))
      report["accumulatedUsage"] = event_notif["accumulatedUsage"];
  } else if (smf_event == "RAT_TY_CH") {
    // No T8 UserPlaneEvent analogue. Pass the SMF string through unchanged
    // for forward-compatibility, carrying ratType when present.
    report["event"] = smf_event;
    if (event_notif.contains("ratType"))
      report["ratType"] = event_notif["ratType"];
  } else {
    // Unknown or unmapped SMF event: pass the string through unchanged
    // (forward-compat). Optional fields are never fabricated.
    Logger::nef_app().debug(
        "map_smf_event_to_userplane: passing through unmapped SMF event '%s'",
        smf_event.c_str());
    report["event"] = smf_event;
  }

  // flowIds applies to every mapped report type. Absent means the report
  // covers all flows (spec note 9), so never fabricate it.
  if (event_notif.contains("flowIds"))
    report["flowIds"] = event_notif["flowIds"];

  return report;
}

// SMF → T8 Session-with-QoS Notification
//------------------------------------------------------------------------------
bool nef_notification_mapper::smf_to_qos_notification(
    const json& smf_notif, json& t8_notif, const std::string& transaction) {
  // Inbound: SMF NsmfEventExposureNotification (TS 29.508 §6.2.6):
  //   { "notifId": "...",
  //     "eventNotifs": [ { "event": "QOS_MON",
  //                         "ulDelays": [...], "dlDelays": [...],
  //                         "timeStamp": "..." }, ... ] }
  //
  // Outbound: T8 UserPlaneNotificationData (TS 29.122):
  //   { "transaction": "<self-URI of the AF subscription>",
  //     "eventReports": [ { "event": "QOS_GUARANTEED",
  //                          "qosMonReports": [ ... ] }, ... ] }
  //
  // "transaction" is the AF subscription's self-URI, i.e. the resource URL
  // returned in the Location/self of the CREATE response. The caller takes it
  // from nef_subscription::get_self().

  if (!smf_notif.contains("eventNotifs") ||
      !smf_notif["eventNotifs"].is_array()) {
    Logger::nef_app().warn(
        "smf_to_qos_notification: missing/invalid 'eventNotifs'");
    t8_notif = make_problem("Bad SMF notification", "eventNotifs not found");
    return false;
  }

  json event_reports = json::array();
  for (const auto& e : smf_notif["eventNotifs"]) {
    const std::string smf_event = e.value("event", "UNKNOWN");
    event_reports.push_back(map_smf_event_to_userplane(smf_event, e));
  }

  if (event_reports.empty()) {
    Logger::nef_app().warn(
        "smf_to_qos_notification: no event reports produced");
    t8_notif = make_problem("Bad SMF notification", "empty eventNotifs");
    return false;
  }

  t8_notif                 = json::object();
  t8_notif["transaction"]  = transaction;
  t8_notif["eventReports"] = event_reports;
  return true;
}

// PCF → T8 Traffic Influence Notification
//------------------------------------------------------------------------------
bool nef_notification_mapper::pcf_to_ti_notification(
    const json& pcf_notif, json& t8_notif, const std::string& sub_id) {
  // Inbound: PCF PolicyAuthorization Notify (TS 29.514 §4.2.3.4):
  //   { "appSessionId": "...",
  //     "evNotifs": [ { "event": "USAGE_REPORT",
  //                      "usgRep": { ... },
  //                      "timeStamp": "..." } ] }
  //
  // Outbound: T8 TrafficInfluenceNotification (TS 29.522 §8.3.2):
  //   { "subscription": "<sub_id>",
  //     "trafficInfluenceNotifs": [ { "dnaiChgType": "...",
  //                                   "targetDnai": "...",
  //                                   "timeStamp": "..." } ] }

  t8_notif["subscription"] = sub_id;

  if (!pcf_notif.contains("evNotifs") || !pcf_notif["evNotifs"].is_array()) {
    Logger::nef_app().warn(
        "pcf_to_ti_notification: missing/invalid 'evNotifs'");
    t8_notif = make_problem("Bad PCF notification", "evNotifs not found");
    return false;
  }

  json ti_notifs = json::array();
  for (const auto& e : pcf_notif["evNotifs"]) {
    json ti_ev;

    std::string pcf_event = e.value("event", "UNKNOWN");
    if (pcf_event == "DNAI_CH_REPORT") {
      ti_ev["dnaiChgType"] = e.value("dnaiChgType", "EARLY");
      if (e.contains("sourceDnai")) ti_ev["sourceDnai"] = e["sourceDnai"];
      if (e.contains("targetDnai")) ti_ev["targetDnai"] = e["targetDnai"];
    } else if (pcf_event == "USAGE_REPORT") {
      ti_ev["usageReport"] = e.value("usgRep", json::object());
    } else {
      ti_ev["event"] = pcf_event;
    }

    if (e.contains("timeStamp")) ti_ev["timeStamp"] = e["timeStamp"];
    ti_notifs.push_back(ti_ev);
  }
  t8_notif["trafficInfluenceNotifs"] = ti_notifs;
  return true;
}

// PCF → T8 Session-with-QoS Notification
//------------------------------------------------------------------------------
bool nef_notification_mapper::pcf_to_qos_notification(
    const json& pcf_notif, json& t8_notif, const std::string& transaction) {
  // Inbound: PCF EventsNotification (TS 29.514 §5.6.2.6):
  //   { "evSubsUri": "...",
  //     "evNotifs": [ { "event": "QOS_NOTIF", "flows": [ ... ] }, ... ],
  //     // Careful: detail payloads live at the TOP LEVEL, not in evNotifs:
  //     "qncReports": [ { "refQosIndication": ..., "notifType": "GUARANTEED",
  //                       "flows": [...] }, ... ],
  //     "usgRep": { ... },               // AccumulatedUsage
  //     "qosMonReports": [ ... ],
  //     "succResourcAllocReports": [...], "failedResourcAllocReports": [...],
  //     "plmnId": { ... }, "accessType": "..." }
  //
  // Outbound: T8 UserPlaneNotificationData (TS 29.122):
  //   { "transaction": "<self-URI of the AF subscription>",
  //     "eventReports": [ { "event": "QOS_GUARANTEED",
  //                          "appliedQosRef": ..., "flowIds": [...] }, ... ] }
  //
  // "transaction" is the AF subscription's self-URI. The caller takes it from
  // nef_subscription::get_self().

  if (!pcf_notif.contains("evNotifs") || !pcf_notif["evNotifs"].is_array()) {
    Logger::nef_app().warn(
        "pcf_to_qos_notification: missing/invalid 'evNotifs'");
    t8_notif = make_problem("Bad PCF notification", "evNotifs not found");
    return false;
  }

  json event_reports = json::array();
  for (const auto& ev : pcf_notif["evNotifs"]) {
    const std::string pcf_event = ev.value("event", "UNKNOWN");

    json report = json::object();

    // PCF AfEvent (TS 29.514) → T8 UserPlaneEvent (TS 29.122) translation.
    if (pcf_event == "SUCCESSFUL_RESOURCES_ALLOCATION") {
      report["event"] = "SUCCESSFUL_RESOURCES_ALLOCATION";
    } else if (pcf_event == "FAILED_RESOURCES_ALLOCATION") {
      report["event"] = "FAILED_RESOURCES_ALLOCATION";
    } else if (pcf_event == "QOS_NOTIF") {
      // T8 has no QOS_NOTIF. Split it on the top-level
      // qncReports[0].notifType; anything but GUARANTEED maps to
      // QOS_NOT_GUARANTEED.
      std::string notif_type =
          "NOT_GUARANTEED";  // → QOS_NOT_GUARANTEED unless overridden
      if (pcf_notif.contains("qncReports") &&
          pcf_notif["qncReports"].is_array() &&
          !pcf_notif["qncReports"].empty()) {
        notif_type = pcf_notif["qncReports"][0].value(
            "notifType", std::string("NOT_GUARANTEED"));
      }
      report["event"] = (notif_type == "GUARANTEED") ? "QOS_GUARANTEED" :
                                                       "QOS_NOT_GUARANTEED";
    } else if (pcf_event == "QOS_MONITORING") {
      report["event"] = "QOS_MONITORING";
    } else if (pcf_event == "USAGE_REPORT") {
      report["event"] = "USAGE_REPORT";
    } else if (pcf_event == "ACCESS_TYPE_CHANGE") {
      report["event"] = "ACCESS_TYPE_CHANGE";
    } else if (pcf_event == "PLMN_CHG") {
      report["event"] = "PLMN_CHG";
    } else if (pcf_event == "OUT_OF_CREDIT") {
      report["event"] = "USAGE_REPORT";  // best effort: no exact T8 peer
    } else {
      report["event"] = pcf_event;  // pass-through
    }

    // Fold in the detail payloads, which sit at the TOP LEVEL of the
    // EventsNotification rather than in the evNotifs entry being mapped.
    if (pcf_notif.contains("qncReports") &&
        pcf_notif["qncReports"].is_array() &&
        !pcf_notif["qncReports"].empty() &&
        pcf_notif["qncReports"][0].contains("refQosIndication")) {
      report["appliedQosRef"] = pcf_notif["qncReports"][0]["refQosIndication"];
    }
    if (pcf_notif.contains("usgRep")) {
      report["accumulatedUsage"] = pcf_notif["usgRep"];
    }
    if (pcf_notif.contains("qosMonReports")) {
      report["qosMonReports"] = pcf_notif["qosMonReports"];
    }
    if (pcf_event == "PLMN_CHG" && pcf_notif.contains("plmnId")) {
      report["plmnId"] = pcf_notif["plmnId"];
    }

    // Per-event flow ids. Absent means the report covers all flows.
    if (ev.contains("flows")) report["flowIds"] = ev["flows"];

    event_reports.push_back(report);
  }

  if (event_reports.empty()) {
    Logger::nef_app().warn(
        "pcf_to_qos_notification: no event reports produced");
    t8_notif = make_problem("Bad PCF notification", "empty evNotifs");
    return false;
  }

  t8_notif                 = json::object();
  t8_notif["transaction"]  = transaction;
  t8_notif["eventReports"] = event_reports;
  return true;
}
