/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_CLIENT_HPP_SEEN
#define FILE_NEF_CLIENT_HPP_SEEN

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "http_client.hpp"
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
 *
 * Most calls come in three flavours. The plain one blocks. The *_async one
 * builds the identical request and issues it without blocking: the callback
 * gets the raw response and never touches nef_app state, so parsing is the
 * caller's job, and a target that cannot be discovered shows up as
 * status_code 0. The *_at_async one takes an endpoint the caller already
 * resolved and skips discovery entirely, which is what makes it safe to call
 * from an oai-http-io continuation without deadlocking the io pool.
 * Only departures from that pattern are noted below.
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

  // Resolvable without a network call (static config or discovery cache)? The
  // callback fires synchronously with a synthetic 200 whose body is a
  // single-instance SearchResult. Otherwise the NRF is queried and its
  // SearchResult is passed through as-is.
  void discover_nf_async(nf_type_t nf_type, oai::sba::response_cb cb);

  // AMF — event-exposure subscription
  bool subscribe_amf_event_exposure(
      const nlohmann::json& subscription_data, std::string& amf_sub_id);

  void subscribe_amf_event_exposure_async(
      const nlohmann::json& subscription_data, oai::sba::response_cb cb);

  bool unsubscribe_amf_event_exposure(const std::string& amf_sub_id);

  void unsubscribe_amf_event_exposure_async(
      const std::string& amf_sub_id, oai::sba::response_cb cb);

  // SMF — event-exposure subscription
  // The caller supplies a fully-formed NsmfEventExposure body; this injects
  // the NEF-chosen correlation id and inbound notification URI before POSTing.
  bool subscribe_smf_event_exposure(
      const nlohmann::json& smf_body, const std::string& notif_id,
      const std::string& notif_uri, std::string& smf_sub_id);

  void subscribe_smf_event_exposure_async(
      const nlohmann::json& smf_body, const std::string& notif_id,
      const std::string& notif_uri, oai::sba::response_cb cb);

  bool unsubscribe_smf_event_exposure(const std::string& smf_sub_id);

  void unsubscribe_smf_event_exposure_async(
      const std::string& smf_sub_id, oai::sba::response_cb cb);

  // Full-replace an existing SMF subscription (notifId/notifUri already
  // embedded by the caller). Nsmf_EventExposure has no PATCH, so PUT is the
  // conformant southbound update for both T8 PUT and T8 PATCH.
  bool update_smf_event_exposure(
      const std::string& smf_sub_id, const nlohmann::json& smf_body);

  // PCF — policy-authorization / BDT-policy
  bool create_pcf_policy_auth(
      const nlohmann::json& request_body, std::string& app_session_id,
      uint32_t& http_code);

  void create_pcf_policy_auth_async(
      const nlohmann::json& request_body, oai::sba::response_cb cb);

  void create_pcf_policy_auth_at_async(
      const std::string& pcf_endpoint, const nlohmann::json& request_body,
      oai::sba::response_cb cb);

  bool update_pcf_policy_auth(
      const std::string& app_session_id, const nlohmann::json& request_body,
      uint32_t& http_code);

  void update_pcf_policy_auth_async(
      const std::string& app_session_id, const nlohmann::json& request_body,
      oai::sba::response_cb cb);

  bool delete_pcf_policy_auth(
      const std::string& app_session_id, uint32_t& http_code);

  void delete_pcf_policy_auth_async(
      const std::string& app_session_id, oai::sba::response_cb cb);

  void delete_pcf_policy_auth_at_async(
      const std::string& pcf_endpoint, const std::string& app_session_id,
      oai::sba::response_cb cb);

  // PUT /npcf-policyauthorization/v1/app-sessions/{id}/events-subscription
  bool subscribe_pcf_events(
      const std::string& app_session_id, const nlohmann::json& ev_subsc_body,
      uint32_t& http_code);

  bool create_pcf_bdt_policy(
      const nlohmann::json& bdt_req, std::string& pcf_bdt_id,
      uint32_t& http_code);

  // A 303 See Other counts as success here.
  void create_pcf_bdt_policy_async(
      const nlohmann::json& bdt_req, oai::sba::response_cb cb);

  bool update_pcf_bdt_policy(
      const std::string& bdt_policy_id, const nlohmann::json& bdt_patch,
      uint32_t& http_code);

  void update_pcf_bdt_policy_async(
      const std::string& bdt_policy_id, const nlohmann::json& bdt_patch,
      oai::sba::response_cb cb);

  bool delete_pcf_bdt_policy(
      const std::string& bdt_policy_id, uint32_t& http_code);

  void delete_pcf_bdt_policy_async(
      const std::string& bdt_policy_id, oai::sba::response_cb cb);

  // UDR — PFD data
  bool udr_put_pfd_data(
      const std::string& app_id, const nlohmann::json& pfd_data);

  void udr_put_pfd_data_async(
      const std::string& app_id, const nlohmann::json& pfd_data,
      oai::sba::response_cb cb);

  // v1 PFD path.
  void udr_put_pfd_data_at_async(
      const std::string& udr_endpoint, const std::string& app_id,
      const nlohmann::json& pfd_data, oai::sba::response_cb cb);

  bool udr_delete_pfd_data(const std::string& app_id);

  void udr_delete_pfd_data_async(
      const std::string& app_id, oai::sba::response_cb cb);

  // v1 PFD path.
  void udr_delete_pfd_data_at_async(
      const std::string& udr_endpoint, const std::string& app_id,
      oai::sba::response_cb cb);

  void udr_get_pfd_data(
      const std::string& app_id, nlohmann::json& result, uint32_t& http_code);

  void udr_get_pfd_data_async(
      const std::string& app_id, oai::sba::response_cb cb);

  // v2 PFD path.
  void udr_get_pfd_data_at_async(
      const std::string& udr_endpoint, const std::string& app_id,
      oai::sba::response_cb cb);

  bool udr_put_influence_data(
      const std::string& ti_id, const nlohmann::json& data,
      uint32_t& http_code);

  void udr_put_influence_data_async(
      const std::string& ti_id, const nlohmann::json& data,
      oai::sba::response_cb cb);

  // v2 influence-data path.
  void udr_put_influence_data_at_async(
      const std::string& udr_endpoint, const std::string& ti_id,
      const nlohmann::json& data, oai::sba::response_cb cb);

  bool udr_delete_influence_data(const std::string& ti_id, uint32_t& http_code);

  void udr_delete_influence_data_async(
      const std::string& ti_id, oai::sba::response_cb cb);

  // v2 influence-data path.
  void udr_delete_influence_data_at_async(
      const std::string& udr_endpoint, const std::string& ti_id,
      oai::sba::response_cb cb);

  // The URL AMF/SMF/PCF post events back to, used as the callback in every
  // southbound subscription:
  //   http://<nef_host>:<port>/nef-notify/v1/notify/<nf_sub_id>
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
