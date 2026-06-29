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

  // If the endpoint is resolvable without a network call (static config or
  // discovery cache), the callback fires synchronously with a synthetic 200
  // whose body is a single-instance SearchResult carrying the endpoint.
  // Otherwise an async GET is issued to the NRF and the raw NRF SearchResult
  // response is delivered to the callback. On failure to resolve a target, the
  // callback fires with status_code 0. The callback does NOT mutate
  // nef_app state; the caller parses the SearchResult.
  void discover_nf_async(nf_type_t nf_type, oai::http::response_cb cb);

  // AMF — event-exposure subscription
  bool subscribe_amf_event_exposure(
      const nlohmann::json& subscription_data, std::string& amf_sub_id);

  // Builds the same POST request as the sync version, then
  // issues a single non-blocking request. The callback receives the raw AMF
  // response (status_code/body); it does NOT mutate nef_app state nor parse the
  // subscription id — the caller does. If AMF cannot be discovered the callback
  // fires with status_code 0 and an empty body.
  void subscribe_amf_event_exposure_async(
      const nlohmann::json& subscription_data, oai::http::response_cb cb);

  bool unsubscribe_amf_event_exposure(const std::string& amf_sub_id);

  // Builds the same DELETE request as the sync version, then issues a single
  // non-blocking request. The callback receives the raw AMF response; it does
  // NOT mutate nef_app state. If AMF cannot be discovered the callback fires
  // with status_code 0.
  void unsubscribe_amf_event_exposure_async(
      const std::string& amf_sub_id, oai::http::response_cb cb);

  // SMF — event-exposure subscription
  // T5: callers build a fully-formed NsmfEventExposure body (with eventSubs,
  // target filters, etc.). This function injects the NEF-chosen correlation id
  // (notif_id) and inbound notification URI (notif_uri) before POSTing to the
  // SMF.
  bool subscribe_smf_event_exposure(
      const nlohmann::json& smf_body, const std::string& notif_id,
      const std::string& notif_uri, std::string& smf_sub_id);

  // Builds the same POST request as the sync version
  // (injecting notifId/notifUri into smf_body), then issues a single
  // non-blocking request. The callback receives the raw SMF response; it does
  // NOT mutate nef_app state nor parse the subscription id. If SMF cannot be
  // discovered the callback fires with status_code 0 and an empty body.
  void subscribe_smf_event_exposure_async(
      const nlohmann::json& smf_body, const std::string& notif_id,
      const std::string& notif_uri, oai::http::response_cb cb);

  bool unsubscribe_smf_event_exposure(const std::string& smf_sub_id);

  // Builds the same DELETE request as the sync version, then issues a single
  // non-blocking request. The callback receives the raw SMF response; it does
  // NOT mutate nef_app state. If SMF cannot be discovered the callback fires
  // with status_code 0.
  void unsubscribe_smf_event_exposure_async(
      const std::string& smf_sub_id, oai::http::response_cb cb);

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

  // Builds the same POST request as the sync version, then
  // issues a single non-blocking request. The callback receives the raw PCF
  // response (status_code/body/headers); it does NOT mutate nef_app state nor
  // parse appSessionId/Location. If PCF cannot be discovered the callback fires
  // with status_code 0 and an empty body.
  void create_pcf_policy_auth_async(
      const nlohmann::json& request_body, oai::http::response_cb cb);

  // Discovery-free variant of create_pcf_policy_auth_async (§B.3): the PCF base
  // endpoint is pre-resolved by the caller (in phase-1 on the dispatcher
  // worker) and passed in; this performs NO discover_nf so it is safe to call
  // from an oai-http-io continuation without risking the io-pool self-deadlock.
  void create_pcf_policy_auth_at_async(
      const std::string& pcf_endpoint, const nlohmann::json& request_body,
      oai::http::response_cb cb);

  bool update_pcf_policy_auth(
      const std::string& app_session_id, const nlohmann::json& request_body,
      uint32_t& http_code);

  // Builds the same PATCH request (merge-patch+json) as the sync version, then
  // issues a single non-blocking request. The callback receives the raw PCF
  // response; it does NOT mutate nef_app state. If PCF cannot be discovered
  // the callback fires with status_code 0.
  void update_pcf_policy_auth_async(
      const std::string& app_session_id, const nlohmann::json& request_body,
      oai::http::response_cb cb);

  bool delete_pcf_policy_auth(
      const std::string& app_session_id, uint32_t& http_code);

  // Builds the same POST .../delete request as the sync version, then issues a
  // single non-blocking request. The callback receives the raw PCF response; it
  // does NOT mutate nef_app state. If PCF cannot be discovered the callback
  // fires with status_code 0.
  void delete_pcf_policy_auth_async(
      const std::string& app_session_id, oai::http::response_cb cb);

  // Discovery-free variant of delete_pcf_policy_auth_async (§B.3): PCF base
  // endpoint pre-resolved by the caller; performs NO discover_nf.
  void delete_pcf_policy_auth_at_async(
      const std::string& pcf_endpoint, const std::string& app_session_id,
      oai::http::response_cb cb);

  // PUT /npcf-policyauthorization/v1/app-sessions/{id}/events-subscription
  bool subscribe_pcf_events(
      const std::string& app_session_id, const nlohmann::json& ev_subsc_body,
      uint32_t& http_code);

  bool create_pcf_bdt_policy(
      const nlohmann::json& bdt_req, std::string& pcf_bdt_id,
      uint32_t& http_code);

  // Builds the same POST request as the sync version, then issues a single
  // non-blocking request. The callback receives the raw PCF response
  // (status_code/body/headers); it does NOT mutate nef_app state nor parse the
  // bdtPolicyId / Location header. A 303 See Other is a success for this call.
  // If PCF cannot be discovered the callback fires with status_code 0.
  void create_pcf_bdt_policy_async(
      const nlohmann::json& bdt_req, oai::http::response_cb cb);

  bool update_pcf_bdt_policy(
      const std::string& bdt_policy_id, const nlohmann::json& bdt_patch,
      uint32_t& http_code);

  // Builds the same PATCH request as the sync version, then issues a single
  // non-blocking request. The callback receives the raw PCF response; it does
  // NOT mutate nef_app state. If PCF cannot be discovered the callback fires
  // with status_code 0.
  void update_pcf_bdt_policy_async(
      const std::string& bdt_policy_id, const nlohmann::json& bdt_patch,
      oai::http::response_cb cb);

  bool delete_pcf_bdt_policy(
      const std::string& bdt_policy_id, uint32_t& http_code);

  // Builds the same DELETE request as the sync version, then issues a single
  // non-blocking request. The callback receives the raw PCF response; it does
  // NOT mutate nef_app state. If PCF cannot be discovered the callback fires
  // with status_code 0.
  void delete_pcf_bdt_policy_async(
      const std::string& bdt_policy_id, oai::http::response_cb cb);

  // UDR — PFD data
  bool udr_put_pfd_data(
      const std::string& app_id, const nlohmann::json& pfd_data);

  // Builds the same PUT request as the sync version, then
  // issues a single non-blocking request. The callback receives the raw UDR
  // response; it does NOT mutate nef_app state. If UDR cannot be discovered the
  // callback fires with status_code 0.
  void udr_put_pfd_data_async(
      const std::string& app_id, const nlohmann::json& pfd_data,
      oai::http::response_cb cb);

  // Discovery-free variant of udr_put_pfd_data_async (§B.3): UDR base endpoint
  // pre-resolved by the caller; performs NO discover_nf. Path is the v1 PFD
  // resource (matches the sync twin udr_put_pfd_data).
  void udr_put_pfd_data_at_async(
      const std::string& udr_endpoint, const std::string& app_id,
      const nlohmann::json& pfd_data, oai::http::response_cb cb);

  bool udr_delete_pfd_data(const std::string& app_id);

  // Builds the same DELETE request as the sync version (v1 PFD path), then
  // issues a single non-blocking request. The callback receives the raw UDR
  // response; it does NOT mutate nef_app state. If UDR cannot be discovered the
  // callback fires with status_code 0.
  void udr_delete_pfd_data_async(
      const std::string& app_id, oai::http::response_cb cb);

  // Discovery-free variant of udr_delete_pfd_data_async (§B.3): UDR base
  // endpoint pre-resolved by the caller; performs NO discover_nf. v1 PFD path.
  void udr_delete_pfd_data_at_async(
      const std::string& udr_endpoint, const std::string& app_id,
      oai::http::response_cb cb);

  void udr_get_pfd_data(
      const std::string& app_id, nlohmann::json& result, uint32_t& http_code);

  // Builds the same GET request as the sync version (v2 PFD path), then issues
  // a single non-blocking request. The callback receives the raw UDR response;
  // it does NOT mutate nef_app state nor parse the body. If UDR cannot be
  // discovered the callback fires with status_code 0.
  void udr_get_pfd_data_async(
      const std::string& app_id, oai::http::response_cb cb);

  // Discovery-free variant of udr_get_pfd_data_async (§B.3): UDR base endpoint
  // pre-resolved by the caller; performs NO discover_nf. v2 PFD path.
  void udr_get_pfd_data_at_async(
      const std::string& udr_endpoint, const std::string& app_id,
      oai::http::response_cb cb);

  bool udr_put_influence_data(
      const std::string& ti_id, const nlohmann::json& data,
      uint32_t& http_code);

  // Builds the same PUT request as the sync version, then
  // issues a single non-blocking request. The callback receives the raw UDR
  // response; it does NOT mutate nef_app state. If UDR cannot be discovered the
  // callback fires with status_code 0.
  void udr_put_influence_data_async(
      const std::string& ti_id, const nlohmann::json& data,
      oai::http::response_cb cb);

  // Discovery-free variant of udr_put_influence_data_async (§B.3): UDR base
  // endpoint pre-resolved by the caller; performs NO discover_nf. v2 influence
  // data path (matches the sync twin udr_put_influence_data).
  void udr_put_influence_data_at_async(
      const std::string& udr_endpoint, const std::string& ti_id,
      const nlohmann::json& data, oai::http::response_cb cb);

  bool udr_delete_influence_data(const std::string& ti_id, uint32_t& http_code);

  // Builds the same DELETE request as the sync version (v2 influence data
  // path), then issues a single non-blocking request. The callback receives the
  // raw UDR response; it does NOT mutate nef_app state. If UDR cannot be
  // discovered the callback fires with status_code 0.
  void udr_delete_influence_data_async(
      const std::string& ti_id, oai::http::response_cb cb);

  // Discovery-free variant of udr_delete_influence_data_async (§B.3): UDR base
  // endpoint pre-resolved by the caller; performs NO discover_nf. v2 path.
  void udr_delete_influence_data_at_async(
      const std::string& udr_endpoint, const std::string& ti_id,
      oai::http::response_cb cb);

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
