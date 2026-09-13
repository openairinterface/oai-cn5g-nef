/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_NOTIFICATION_MAPPER_HPP_SEEN
#define FILE_NEF_NOTIFICATION_MAPPER_HPP_SEEN

#include <nlohmann/json.hpp>
#include <string>

namespace oai {
namespace nef {
namespace app {

/*
 * Converts southbound NF notifications (AMF / SMF / PCF) into the northbound
 * T8 format sent to the AF. Every method is static and takes the raw 5GC JSON
 * in and the mapped T8 JSON out. On a parse error or a missing mandatory
 * field it returns false and writes an RFC 7807 problem-detail instead.
 */
class nef_notification_mapper {
 public:
  nef_notification_mapper()                               = delete;
  nef_notification_mapper(const nef_notification_mapper&) = delete;
  nef_notification_mapper& operator=(const nef_notification_mapper&) = delete;

  // AMF EventExposure (TS 29.518 §6.3.4.3.3) -> T8 MonitoringEventNotification
  // (TS 29.122 §8.4.4.3.2). sub_id is embedded in the T8 object.
  static bool amf_to_monitoring_notification(
      const nlohmann::json& amf_notif, nlohmann::json& t8_notif,
      const std::string& sub_id);

  // SMF NsmfEventExposureNotification (TS 29.508) -> T8
  // UserPlaneNotificationData (TS 29.122): { transaction, eventReports[] }.
  // transaction is the AF subscription's self-URI, i.e. the resource URL
  // handed back in the CREATE Location header. False if no report came out.
  static bool smf_to_qos_notification(
      const nlohmann::json& smf_notif, nlohmann::json& t8_notif,
      const std::string& transaction);

  // PCF policy notification -> T8 TrafficInfluenceNotification.
  static bool pcf_to_ti_notification(
      const nlohmann::json& pcf_notif, nlohmann::json& t8_notif,
      const std::string& sub_id);

  // PCF EventsNotification (TS 29.514 §5.6.2.6) -> T8
  // UserPlaneNotificationData (TS 29.122), translating AfEvent values to
  // UserPlaneEvent values.
  //
  // Worth knowing: PCF's "evNotifs" array holds only {event, flows[]}. The
  // detail payloads (qncReports, usgRep, qosMonReports, plmnId, accessType,
  // succ/failedResourcAllocReports) sit at the top level of the notification,
  // so this folds them into each individual report.
  static bool pcf_to_qos_notification(
      const nlohmann::json& pcf_notif, nlohmann::json& t8_notif,
      const std::string& transaction);
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_NOTIFICATION_MAPPER_HPP_SEEN */
