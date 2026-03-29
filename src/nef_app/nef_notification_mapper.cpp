/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License. You may obtain a copy of the
 * License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "nef_notification_mapper.hpp"
#include "logger.hpp"

using namespace oai::nef::app;
using json = nlohmann::json;

// Helper: RFC 7807 problem-detail
static json make_problem(const std::string& title, const std::string& detail) {
  return json{{"title", title}, {"detail", detail}, {"status", 0}};
}

// AMF → T8 Monitoring Event Notification
bool nef_notification_mapper::amf_to_monitoring_notification(
    const json& amf_notif, json& t8_notif, const std::string& sub_id) {
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

  t8_notif["subscription"] = sub_id;

  if (!amf_notif.contains("reportList") || !amf_notif["reportList"].is_array()) {
    Logger::nef_app().warn(
        "amf_to_monitoring_notification: missing/invalid 'reportList'");
    t8_notif = make_problem("Bad AMF notification", "reportList not found");
    return false;
  }

  json reports = json::array();
  for (const auto& r : amf_notif["reportList"]) {
    json t8_report;

    // Translate AMF event type to T8 monitoringType
    std::string amf_type = r.value("type", "UNKNOWN");
    if (amf_type == "LOCATION_REPORT") {
      t8_report["monitoringType"] = "LOCATION_REPORTING";
      if (r.contains("state")) t8_report["locationInfo"] = r["state"];
    } else if (amf_type == "UE_REACHABILITY_FOR_SMS") {
      t8_report["monitoringType"] = "UE_REACHABILITY";
      t8_report["reachabilityForSms"] = true;
    } else if (amf_type == "UE_REACHABILITY_FOR_DATA") {
      t8_report["monitoringType"] = "UE_REACHABILITY";
      t8_report["reachabilityForData"] = true;
    } else if (amf_type == "PDU_SESSION_STATUS") {
      t8_report["monitoringType"] = "PDU_SESSION_STATUS";
      if (r.contains("pduSessionStatusList"))
        t8_report["pduSessionStatus"] = r["pduSessionStatusList"];
    } else {
      // Pass-through for unknown types
      t8_report["monitoringType"] = amf_type;
      if (r.contains("state")) t8_report["stateInfo"] = r["state"];
    }

    // Common fields
    if (r.contains("supi"))      t8_report["supi"]      = r["supi"];
    if (r.contains("timeStamp")) t8_report["timeStamp"] = r["timeStamp"];
    if (r.contains("gpsi"))      t8_report["gpsi"]      = r["gpsi"];

    reports.push_back(t8_report);
  }
  t8_notif["monitoringEventReports"] = reports;
  return true;
}

// SMF → T8 Session-with-QoS Notification
bool nef_notification_mapper::smf_to_qos_notification(
    const json& smf_notif, json& t8_notif, const std::string& sub_id) {
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

  t8_notif["subscription"] = sub_id;

  if (!smf_notif.contains("eventNotifs") || !smf_notif["eventNotifs"].is_array()) {
    Logger::nef_app().warn(
        "smf_to_qos_notification: missing/invalid 'eventNotifs'");
    t8_notif = make_problem("Bad SMF notification", "eventNotifs not found");
    return false;
  }

  json ev_notifs = json::array();
  for (const auto& e : smf_notif["eventNotifs"]) {
    json t8_ev;

    std::string smf_event = e.value("event", "UNKNOWN");
    if (smf_event == "QOS_MONITORING") {
      t8_ev["event"] = "QOS_GUARANTEED";
      if (e.contains("qosMonitoringMeasurement"))
        t8_ev["qosMonInfo"] = e["qosMonitoringMeasurement"];
    } else if (smf_event == "PDU_SESSION_RELEASE") {
      t8_ev["event"] = "SESSION_TERMINATION";
    } else {
      t8_ev["event"] = smf_event;
    }

    if (e.contains("supi"))      t8_ev["supi"]      = e["supi"];
    if (e.contains("timeStamp")) t8_ev["timeStamp"] = e["timeStamp"];
    if (e.contains("pduSeId"))   t8_ev["pduSeId"]   = e["pduSeId"];

    ev_notifs.push_back(t8_ev);
  }
  t8_notif["evNotifs"] = ev_notifs;
  return true;
}

// PCF → T8 Traffic Influence Notification
bool nef_notification_mapper::pcf_to_ti_notification(
    const json& pcf_notif, json& t8_notif, const std::string& sub_id) {
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
