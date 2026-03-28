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

/*! \file nef_notification_mapper.hpp
 \brief Translates 5GC NF notification formats to NEF northbound (T8) formats.
        Mirrors the "nrf → AF notification forwarding" pattern in nrf_app.
 \author OAI
 \date   2024
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
 * ───────────────────────
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

  // ── Monitoring Event (AMF → NEF → AF) ──────────────────────────────────
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

  // ── Session-with-QoS (SMF → NEF → AF) ──────────────────────────────────
  /*
   * Map an SMF event notification into a T8 AsSessionWithQoSEventNotification.
   *
   * @param [in]  smf_notif   Raw JSON from SMF Nsmf_EventExposure_Notify.
   * @param [out] t8_notif    Mapped T8 notification JSON.
   * @param [in]  sub_id      NEF subscription-id to embed.
   * @return true on success.
   */
  static bool smf_to_qos_notification(
      const nlohmann::json& smf_notif, nlohmann::json& t8_notif,
      const std::string& sub_id);

  // ── Traffic Influence / Policy (PCF → NEF → AF) ─────────────────────────
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
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_NOTIFICATION_MAPPER_HPP_SEEN */
