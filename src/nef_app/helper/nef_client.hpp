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
 *  2. NF discovery: a statically configured address first, then the NRF.
 *  3. Southbound subscriptions to AMF/SMF/PCF/UDR.
 *  4. Forwarding notifications to AFs.
 *
 * Most calls come in three forms:
 *
 *  - foo() blocks until the peer answers.
 *
 *  - foo_async() builds the same request and sends it without waiting. The
 *    callback gets the raw response and nothing else: it never touches nef_app
 *    state, so parsing the body is the caller's job. A target that cannot be
 *    discovered arrives as status_code 0. Note that the endpoint is looked up
 *    with the blocking discover_nf(); only the southbound call itself is
 *    asynchronous.
 *
 *  - foo_at_async() takes an endpoint the caller has already resolved, and so
 *    does no discovery at all. That is what makes it safe to call from an
 *    oai-http-io continuation: discovery there would block an io-pool thread
 *    and deadlock the pool.
 *
 * The comments below only note where a call differs from this pattern.
 *
 * Derives from oai::sba::nf_service, which runs the NRF procedures:
 * registration, de-registration and discovery. This class supplies the NEF
 * policy for them through the base's protected hooks: the register_nrf config
 * gate, looking up local config before the NRF, its own SearchResult selection,
 * sending with retries and the circuit breaker, the 8080 default SBI port and
 * the 200/201 registration success test. The discovery cache, with its TTL,
 * belongs to the base; discover_nf_async() shares it by going through the same
 * hooks.
 *
 * The heartbeat is the one NRF procedure implemented here rather than in the
 * base. nef_app runs it on a 50 s task, to match the heartBeatTimer this NF
 * advertises; the base would use its own 10 s timer instead. See the notes in
 * nef_client.cpp.
 */
class nef_client : public oai::sba::nf_service {
 public:
  nef_client(
      const std::shared_ptr<oai::sba::nf_event>& ev,
      const std::shared_ptr<oai::nghttp2::http_client>& client_inst);
  ~nef_client() override;

  nef_client(nef_client const&)     = delete;
  void operator=(nef_client const&) = delete;

  // NRF registration. All three do nothing and return true when register_nrf
  // is false in the config.
  //
  // The first two build the NEF-specific arguments and pass the work to
  // nf_service. The heartbeat sends its own PATCH, and nef_app calls it.
  bool register_to_nrf();
  bool deregister_from_nrf();
  bool send_heartbeat_to_nrf();

  // NF discovery
  bool discover_nf(nf_type_t nf_type, std::string& nf_endpoint);

  // Non-blocking NF discovery. The callback is always called, with one of three
  // outcomes:
  //  - resolved from static config or the discovery cache: no network call at
  //    all, and the callback runs synchronously with a synthetic 200 whose body
  //    is a single-instance SearchResult;
  //  - resolved by asking the NRF: the NRF's own SearchResult, passed through
  //    as-is;
  //  - not resolvable: status_code 0 with an empty body.
  void discover_nf_async(nf_type_t nf_type, oai::nghttp2::response_cb cb);

  // AMF: event-exposure subscription
  bool subscribe_amf_event_exposure(
      const nlohmann::json& subscription_data, std::string& amf_sub_id);

  void subscribe_amf_event_exposure_async(
      const nlohmann::json& subscription_data, oai::nghttp2::response_cb cb);

  bool unsubscribe_amf_event_exposure(const std::string& amf_sub_id);

  void unsubscribe_amf_event_exposure_async(
      const std::string& amf_sub_id, oai::nghttp2::response_cb cb);

  // SMF: event-exposure subscription
  // The caller supplies a complete NsmfEventExposure body. This adds the
  // NEF-chosen correlation id and the inbound notification URI, then sends the
  // POST.
  bool subscribe_smf_event_exposure(
      const nlohmann::json& smf_body, const std::string& notif_id,
      const std::string& notif_uri, std::string& smf_sub_id);

  void subscribe_smf_event_exposure_async(
      const nlohmann::json& smf_body, const std::string& notif_id,
      const std::string& notif_uri, oai::nghttp2::response_cb cb);

  bool unsubscribe_smf_event_exposure(const std::string& smf_sub_id);

  void unsubscribe_smf_event_exposure_async(
      const std::string& smf_sub_id, oai::nghttp2::response_cb cb);

  // Replaces an existing SMF subscription in full. The caller must already
  // have put notifId/notifUri in the body.
  //
  // Nsmf_EventExposure defines no PATCH, so a PUT is the conformant way to
  // update the subscription. Nothing calls this today.
  bool update_smf_event_exposure(
      const std::string& smf_sub_id, const nlohmann::json& smf_body);

  // PCF: policy authorization and BDT policy
  bool create_pcf_policy_auth(
      const nlohmann::json& request_body, std::string& app_session_id,
      uint32_t& http_code);

  void create_pcf_policy_auth_async(
      const nlohmann::json& request_body, oai::nghttp2::response_cb cb);

  void create_pcf_policy_auth_at_async(
      const std::string& pcf_endpoint, const nlohmann::json& request_body,
      oai::nghttp2::response_cb cb);

  bool update_pcf_policy_auth(
      const std::string& app_session_id, const nlohmann::json& request_body,
      uint32_t& http_code);

  void update_pcf_policy_auth_async(
      const std::string& app_session_id, const nlohmann::json& request_body,
      oai::nghttp2::response_cb cb);

  bool delete_pcf_policy_auth(
      const std::string& app_session_id, uint32_t& http_code);

  void delete_pcf_policy_auth_async(
      const std::string& app_session_id, oai::nghttp2::response_cb cb);

  void delete_pcf_policy_auth_at_async(
      const std::string& pcf_endpoint, const std::string& app_session_id,
      oai::nghttp2::response_cb cb);

  // PUT /npcf-policyauthorization/v1/app-sessions/{id}/events-subscription
  bool subscribe_pcf_events(
      const std::string& app_session_id, const nlohmann::json& ev_subsc_body,
      uint32_t& http_code);

  bool create_pcf_bdt_policy(
      const nlohmann::json& bdt_req, std::string& pcf_bdt_id,
      uint32_t& http_code);

  // A 303 See Other counts as success here.
  void create_pcf_bdt_policy_async(
      const nlohmann::json& bdt_req, oai::nghttp2::response_cb cb);

  bool update_pcf_bdt_policy(
      const std::string& bdt_policy_id, const nlohmann::json& bdt_patch,
      uint32_t& http_code);

  void update_pcf_bdt_policy_async(
      const std::string& bdt_policy_id, const nlohmann::json& bdt_patch,
      oai::nghttp2::response_cb cb);

  bool delete_pcf_bdt_policy(
      const std::string& bdt_policy_id, uint32_t& http_code);

  void delete_pcf_bdt_policy_async(
      const std::string& bdt_policy_id, oai::nghttp2::response_cb cb);

  // UDR: PFD data and influence data
  bool udr_put_pfd_data(
      const std::string& app_id, const nlohmann::json& pfd_data);

  void udr_put_pfd_data_async(
      const std::string& app_id, const nlohmann::json& pfd_data,
      oai::nghttp2::response_cb cb);

  // Uses the v1 PFD path.
  void udr_put_pfd_data_at_async(
      const std::string& udr_endpoint, const std::string& app_id,
      const nlohmann::json& pfd_data, oai::nghttp2::response_cb cb);

  bool udr_delete_pfd_data(const std::string& app_id);

  void udr_delete_pfd_data_async(
      const std::string& app_id, oai::nghttp2::response_cb cb);

  // Uses the v1 PFD path.
  void udr_delete_pfd_data_at_async(
      const std::string& udr_endpoint, const std::string& app_id,
      oai::nghttp2::response_cb cb);

  void udr_get_pfd_data(
      const std::string& app_id, nlohmann::json& result, uint32_t& http_code);

  void udr_get_pfd_data_async(
      const std::string& app_id, oai::nghttp2::response_cb cb);

  // Uses the v2 PFD path.
  void udr_get_pfd_data_at_async(
      const std::string& udr_endpoint, const std::string& app_id,
      oai::nghttp2::response_cb cb);

  bool udr_put_influence_data(
      const std::string& ti_id, const nlohmann::json& data,
      uint32_t& http_code);

  void udr_put_influence_data_async(
      const std::string& ti_id, const nlohmann::json& data,
      oai::nghttp2::response_cb cb);

  // Uses the v2 influence-data path.
  void udr_put_influence_data_at_async(
      const std::string& udr_endpoint, const std::string& ti_id,
      const nlohmann::json& data, oai::nghttp2::response_cb cb);

  bool udr_delete_influence_data(const std::string& ti_id, uint32_t& http_code);

  void udr_delete_influence_data_async(
      const std::string& ti_id, oai::nghttp2::response_cb cb);

  // Uses the v2 influence-data path.
  void udr_delete_influence_data_at_async(
      const std::string& udr_endpoint, const std::string& ti_id,
      oai::nghttp2::response_cb cb);

  // Builds NEF's notification callback URL for one NF subscription:
  //   http://<nef_host>:<port>/nef-notify/v1/notify/<nf_sub_id>
  // Nothing calls it today. The AMF subscribe builds its fixed
  // .../v1/notify/amf callback inline instead.
  static std::string get_nef_notify_uri(const std::string& nf_sub_id);

  // Forward notification to AF
  bool forward_notification_to_af(
      const std::string& af_notif_uri, const nlohmann::json& payload);

 protected:
  // NEF policy for the NRF procedures that nf_service runs.

  // A single config switch, register_nrf, controls both.
  bool nrf_registration_enabled() const override;
  bool nrf_discovery_enabled() const override;

  // AMF/SMF/PCF/UDR may be pinned in nef.yaml. A pinned address wins over
  // whatever the NRF would answer.
  bool resolve_endpoint_from_config(
      const std::string& target_nf_type, const std::string& service_name,
      std::string& endpoint) override;

  bool handle_discovery_response(
      const oai::nghttp2::response& search_result_resp,
      const std::string& target_nf_type, const std::string& service_name,
      std::string& endpoint) override;

  // Registration and discovery go through sbi_call_with_retry and feed the SBI
  // circuit breaker. De-registration (at shutdown) and the heartbeat are sent
  // once, with no retry.
  oai::nghttp2::response send_with_policy(
      oai::sba::nrf_call_kind kind, const oai::common::sbi::method_e& method,
      const oai::nghttp2::request& req) override;

  uint16_t default_sbi_port() const override { return 8080; }

  // Only the status code is checked. The NRF this NEF registers against answers
  // the PUT with a body that does not always include nfStatus.
  bool registration_succeeded(
      const oai::nghttp2::response& resp) const override;

  // Only logs the outcome. nef_app owns the heartbeat and the re-registration
  // schedule, so neither of the base's timers is started here.
  void on_registration_outcome(
      bool success, const oai::nghttp2::response& resp) override;

 private:
  // Base members that do not apply to NEF. They are made private here so a
  // caller cannot use them by accident:
  //  - generate_uuid: the instance id must not change once registered;
  //  - generate_nf_profile: an empty stub in the base, while register_to_nrf()
  //    above builds the real NEF profile;
  //  - start_event_nf_heartbeat, trigger_nf_heartbeat_procedure: nef_app runs
  //    the 50 s heartbeat task this NF advertises;
  //  - start/stop_nrf_registration_retry,
  //    trigger_nrf_registration_retry_procedure: re-registration comes from
  //    nef_app's heartbeat-failure path, not from a timer this class owns.
  //
  // register_to_nrf and discover_nf need no entry here, because the overloads
  // above already hide them. deregister_to_nrf is not listed because it does
  // apply to NEF: deregister_from_nrf() calls it.
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
