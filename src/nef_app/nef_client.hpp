/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_CLIENT_HPP_SEEN
#define FILE_NEF_CLIENT_HPP_SEEN

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "nef.h"

namespace oai {
namespace nef {
namespace app {

/**
 * HTTP/SBI client used by NEF for:
 *  1. NRF registration / heartbeat / de-registration.
 *  2. NF discovery via NRF (with config-based fallback).
 *  3. Southbound subscriptions to AMF/SMF/PCF/UDR.
 *  4. Forwarding notifications to AFs.
 */
class nef_client {
 public:
  nef_client();
  virtual ~nef_client();

  nef_client(nef_client const&) = delete;
  void operator=(nef_client const&) = delete;

  // NRF registration
  bool register_to_nrf();
  bool deregister_from_nrf();
  bool send_heartbeat_to_nrf();

  // NF discovery
  bool discover_nf(nf_type_t nf_type, std::string& nf_endpoint);

  // AMF — event-exposure subscription
  bool subscribe_amf_event_exposure(
      const nlohmann::json& subscription_data, std::string& amf_sub_id);

  bool unsubscribe_amf_event_exposure(const std::string& amf_sub_id);

  // SMF — event-exposure subscription
  // T5: callers build a fully-formed NsmfEventExposure body (with eventSubs,
  // target filters, etc.). This function injects the NEF-chosen correlation id
  // (notif_id) and inbound notification URI (notif_uri) before POSTing to the
  // SMF.
  bool subscribe_smf_event_exposure(
      const nlohmann::json& smf_body, const std::string& notif_id,
      const std::string& notif_uri, std::string& smf_sub_id);

  bool unsubscribe_smf_event_exposure(const std::string& smf_sub_id);

  // T8: full-replace an existing SMF event-exposure subscription. Issues
  // PUT /nsmf-event-exposure/v1/subscriptions/{smf_sub_id} with a fully-formed
  // NsmfEventExposure body (notifId/notifUri already embedded by the caller).
  // Returns true on a 2xx response. Nsmf_EventExposure has no PATCH, so PUT is
  // the conformant southbound update action for both T8 PUT and PATCH.
  bool update_smf_event_exposure(
      const std::string& smf_sub_id, const nlohmann::json& smf_body);

  // PCF — policy-authorization / BDT-policy
  bool create_pcf_policy_auth(
      const nlohmann::json& request_body, std::string& app_session_id,
      uint32_t& http_code);

  bool update_pcf_policy_auth(
      const std::string& app_session_id, const nlohmann::json& request_body,
      uint32_t& http_code);

  bool delete_pcf_policy_auth(
      const std::string& app_session_id, uint32_t& http_code);

  // PUT /npcf-policyauthorization/v1/app-sessions/{id}/events-subscription
  bool subscribe_pcf_events(
      const std::string& app_session_id, const nlohmann::json& ev_subsc_body,
      uint32_t& http_code);

  bool create_pcf_bdt_policy(
      const nlohmann::json& bdt_req, std::string& pcf_bdt_id,
      uint32_t& http_code);

  bool update_pcf_bdt_policy(
      const std::string& bdt_policy_id, const nlohmann::json& bdt_patch,
      uint32_t& http_code);

  bool delete_pcf_bdt_policy(
      const std::string& bdt_policy_id, uint32_t& http_code);

  // UDR — PFD data
  bool udr_put_pfd_data(
      const std::string& app_id, const nlohmann::json& pfd_data);

  bool udr_delete_pfd_data(const std::string& app_id);

  void udr_get_pfd_data(
      const std::string& app_id, nlohmann::json& result, uint32_t& http_code);

  bool udr_put_influence_data(
      const std::string& ti_id, const nlohmann::json& data,
      uint32_t& http_code);

  bool udr_delete_influence_data(const std::string& ti_id, uint32_t& http_code);

  // NEF own notification URL (used as callback in southbound subscriptions)
  /**
   * Build the URL that AMF/SMF/PCF should POST to when they have an event for
   * a given NF subscription.  Shape:
   *   http://<nef_host>:<port>/nef-notify/v1/notify/<nf_sub_id>
   */
  static std::string get_nef_notify_uri(const std::string& nf_sub_id);

  // Forward notification to AF
  bool forward_notification_to_af(
      const std::string& af_notif_uri, const nlohmann::json& payload);

 private:
  std::string m_nef_instance_id;  ///< UUID generated at construction
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_CLIENT_HPP_SEEN */
