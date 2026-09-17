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
#include "nf_event.hpp"
#include "nf_service.hpp"

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
 * Most calls come in three flavours:
 *
 *  - foo() blocks until the peer answers.
 *
 *  - foo_async() builds the identical request and sends it without waiting.
 *    The callback gets the raw response and nothing else: it never touches
 *    nef_app state, so parsing the body is the caller's job. A target that
 *    cannot be discovered arrives as status_code 0. Note that resolving the
 *    endpoint still goes through the blocking discover_nf(); only the
 *    southbound call itself is asynchronous.
 *
 *  - foo_at_async() takes an endpoint the caller has already resolved, and so
 *    does no discovery at all. That is what makes it safe to call from an
 *    oai-http-io continuation: discovery there would block an io-pool thread
 *    and deadlock the pool.
 *
 * Only departures from that pattern are noted below.
 *
 * Derives from oai::sba::nf_service, which owns the NRF procedures:
 * registration, de-registration and discovery. This class supplies the NEF
 * policy for them through the base's protected hooks, namely the register_nrf
 * config gate, the local-config-before-NRF resolution order, its own
 * SearchResult selection, the retry/circuit-breaker transport, NEF's 8080
 * default SBI port and its 200/201 registration success test. The TTL
 * discovery cache is the base's; discover_nf_async() shares it by going
 * through the same hooks.
 *
 * The heartbeat is the one NRF procedure that stays here. nef_app drives it on
 * a 50 s task, to match the heartBeatTimer this NF advertises; the base would
 * drive its own 10 s timer instead. See the notes in nef_client.cpp.
 */
class nef_client : public oai::sba::nf_service {
 public:
  nef_client(
      const std::shared_ptr<oai::sba::nf_event>& ev,
      const std::shared_ptr<oai::sba::http_client>& client_inst);
  ~nef_client() override;

  nef_client(nef_client const&) = delete;
  void operator=(nef_client const&) = delete;

  // NRF registration. All three do nothing and return true when register_nrf
  // is false in the config.
  //
  // The first two build the NEF-specific arguments and hand the procedure to
  // nf_service. The heartbeat does not: nef_app drives it.
  bool register_to_nrf();
  bool deregister_from_nrf();
  bool send_heartbeat_to_nrf();

  // NF discovery
  bool discover_nf(nf_type_t nf_type, std::string& nf_endpoint);

  // Non-blocking NF discovery. The callback always fires, with one of three
  // outcomes:
  //  - resolved from static config or the discovery cache: no network call at
  //    all, and the callback fires synchronously with a synthetic 200 whose
  //    body is a single-instance SearchResult;
  //  - resolved by asking the NRF: the NRF's own SearchResult, passed through
  //    as-is;
  //  - not resolvable: status_code 0 with an empty body.
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
  // The caller supplies a fully-formed NsmfEventExposure body. This injects
  // the NEF-chosen correlation id and the inbound notification URI, then
  // POSTs.
  bool subscribe_smf_event_exposure(
      const nlohmann::json& smf_body, const std::string& notif_id,
      const std::string& notif_uri, std::string& smf_sub_id);

  void subscribe_smf_event_exposure_async(
      const nlohmann::json& smf_body, const std::string& notif_id,
      const std::string& notif_uri, oai::sba::response_cb cb);

  bool unsubscribe_smf_event_exposure(const std::string& smf_sub_id);

  void unsubscribe_smf_event_exposure_async(
      const std::string& smf_sub_id, oai::sba::response_cb cb);

  // Full-replace an existing SMF subscription; the caller has already
  // embedded notifId/notifUri in the body.
  //
  // Nsmf_EventExposure defines no PATCH, so PUT is the conformant southbound
  // update for both a T8 PUT and a T8 PATCH.
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

  // The URL AMF/SMF/PCF post events back to. It goes into every southbound
  // subscription as the notification callback:
  //   http://<nef_host>:<port>/nef-notify/v1/notify/<nf_sub_id>
  static std::string get_nef_notify_uri(const std::string& nf_sub_id);

  // Forward notification to AF
  bool forward_notification_to_af(
      const std::string& af_notif_uri, const nlohmann::json& payload);

 protected:
  // NEF policy for the procedures nf_service owns.

  // A single config switch, register_nrf, gates both.
  bool nrf_registration_enabled() const override;
  bool nrf_discovery_enabled() const override;

  // AMF/SMF/PCF/UDR may be pinned in nef.yaml. A pinned address wins over
  // whatever the NRF would answer.
  bool resolve_endpoint_from_config(
      const std::string& target_nf_type, const std::string& service_name,
      std::string& endpoint) override;

  bool handle_discovery_response(
      const oai::sba::response& search_result_resp,
      const std::string& target_nf_type, const std::string& service_name,
      std::string& endpoint) override;

  // Registration and discovery go through sbi_call_with_retry and feed the
  // SBI circuit breaker. De-registration (shutdown) and the heartbeat are
  // single shots.
  oai::sba::response send_with_policy(
      oai::sba::nrf_call_kind kind, const oai::common::sbi::method_e& method,
      const oai::sba::request& req) override;

  uint16_t default_sbi_port() const override { return 8080; }

  // The status code alone decides. The NRF this NEF registers against answers
  // the PUT with a body that does not always carry nfStatus.
  bool registration_succeeded(const oai::sba::response& resp) const override;

  // Logs the outcome, and nothing more. nef_app owns the heartbeat and the
  // re-registration schedule, so neither of the base's timers is armed here.
  void on_registration_outcome(
      bool success, const oai::sba::response& resp) override;

 private:
  // Base members that do not apply to NEF. They are kept off this class's
  // public surface so a caller cannot reach them by accident:
  //  - generate_uuid: the instance id must not change once registered;
  //  - generate_nf_profile: an empty stub in the base, while register_to_nrf()
  //    above builds the real NEF profile;
  //  - the heartbeat pair: nef_app drives the 50 s task this NF advertises;
  //  - the retry trio: re-registration runs off nef_app's heartbeat-failure
  //    path, not off a timer this class owns.
  //
  // register_to_nrf and discover_nf need no entry here, because the overloads
  // above already hide them. deregister_to_nrf is no longer fenced off at all:
  // deregister_from_nrf() calls it.
  using oai::sba::nf_service::generate_nf_profile;
  using oai::sba::nf_service::generate_uuid;
  using oai::sba::nf_service::start_event_nf_heartbeat;
  using oai::sba::nf_service::start_nrf_registration_retry;
  using oai::sba::nf_service::stop_nrf_registration_retry;
  using oai::sba::nf_service::trigger_nf_heartbeat_procedure;
  using oai::sba::nf_service::trigger_nrf_registration_retry_procedure;
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_CLIENT_HPP_SEEN */
