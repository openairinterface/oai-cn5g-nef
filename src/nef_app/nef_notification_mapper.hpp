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
 * nef_notification_mapper
 *
 * Stateless utility class that converts southbound NF notification payloads
 * (AMF / SMF / PCF) into the northbound format that is sent to the AF via the
 * NEF T8 reference point.
 *
 * Design principles:
 *  - All methods are static — no instance state needed.
 *  - Input  : raw JSON notification as received from the 5GC NF.
 *  - Output : mapped JSON suitable for forwarding to the AF.
 *  - On parse error / missing mandatory fields the method returns false and
 *    the output json is set to an RFC 7807 problem-detail object.
 */
class nef_notification_mapper {
 public:
  nef_notification_mapper()                               = delete;
  nef_notification_mapper(const nef_notification_mapper&) = delete;
  nef_notification_mapper& operator=(const nef_notification_mapper&) = delete;

  // Monitoring Event (AMF → NEF → AF)
  /*
   * Map an AMF EventExposure notification (3GPP TS 29.518 §6.3.4.3.3) into a
   * T8 MonitoringEventNotification (3GPP TS 29.122 §8.4.4.3.2).
   *
   * @param [in]  amf_notif   Raw JSON from AMF Namf_EventExposure_Notify.
   * @param [out] t8_notif    Mapped T8 MonitoringEventNotification JSON.
   * @param [in]  sub_id      NEF subscription-id to embed in the T8 object.
   * @return true on success, false if mandatory fields are absent.
   */
  static bool amf_to_monitoring_notification(
      const nlohmann::json& amf_notif, nlohmann::json& t8_notif,
      const std::string& sub_id);

  // Session-with-QoS (SMF → NEF → AF)
  /*
   * Map an SMF NsmfEventExposureNotification (TS 29.508) into a T8
   * UserPlaneNotificationData (TS 29.122): { transaction, eventReports[] }.
   *
   * @param [in]  smf_notif    Raw JSON from SMF Nsmf_EventExposure_Notify.
   * @param [out] t8_notif     Mapped T8 UserPlaneNotificationData JSON.
   * @param [in]  transaction  Self-URI of the AF subscription (the resource
   *                           URL returned in the CREATE Location/self header),
   *                           used as the "transaction" reference.
   * @return true on success, false if no event reports could be produced.
   */
  static bool smf_to_qos_notification(
      const nlohmann::json& smf_notif, nlohmann::json& t8_notif,
      const std::string& transaction);

  // Traffic Influence / Policy (PCF → NEF → AF)
  /*
   * Map a PCF policy notification into a T8 TrafficInfluenceNotification.
   *
   * @param [in]  pcf_notif   Raw JSON from PCF Npcf_PolicyAuthorization_Notify.
   * @param [out] t8_notif    Mapped T8 notification JSON.
   * @param [in]  sub_id      NEF subscription-id to embed.
   * @return true on success.
   */
  static bool pcf_to_ti_notification(
      const nlohmann::json& pcf_notif, nlohmann::json& t8_notif,
      const std::string& sub_id);

  // Session-with-QoS (PCF → NEF → AF)
  /*
   * Map a PCF EventsNotification (TS 29.514 §5.6.2.6, type EventsNotification)
   * into a T8 UserPlaneNotificationData (TS 29.122):
   *   { transaction, eventReports[] }.
   *
   * PCF AfEvent values (TS 29.514) are translated to T8 UserPlaneEvent values
   * (TS 29.122). The PCF "evNotifs" array carries only {event, flows[]}; the
   * detail payloads (qncReports, usgRep, qosMonReports, plmnId, accessType,
   * succ/failedResourcAllocReports) live at the TOP LEVEL of EventsNotification
   * and are folded into each report here.
   *
   * @param [in]  pcf_notif    Raw JSON from PCF
   * Npcf_PolicyAuthorization_Notify.
   * @param [out] t8_notif     Mapped T8 UserPlaneNotificationData JSON.
   * @param [in]  transaction  Self-URI of the AF subscription, used as the
   *                           "transaction" reference.
   * @return true on success, false if no event reports could be produced.
   */
  static bool pcf_to_qos_notification(
      const nlohmann::json& pcf_notif, nlohmann::json& t8_notif,
      const std::string& transaction);
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_NOTIFICATION_MAPPER_HPP_SEEN */
