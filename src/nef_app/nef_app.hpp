/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_APP_HPP_SEEN
#define FILE_NEF_APP_HPP_SEEN

#include <atomic>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/signals2.hpp>
#include <nlohmann/json.hpp>

#include "AsSessionWithQoSSubscription.h"
#include "BdtPolicy.h"
#include "NefEventExposureSubsc.h"
#include "PfdDataForApp.h"
#include "PfdSubscription.h"
#include "nef.h"
#include "nef_af_profile.hpp"
#include "nef_event.hpp"
#include "nef_notification_queue.hpp"
#include "nef_request_task.hpp"  // response_sink
#include "nef_subscription.hpp"
#include "uint_generator.hpp"

namespace bs2 = boost::signals2;

namespace oai {
namespace sba {
// Only needed for the cont_* signatures below; the definition lives in
// sba/http_definitions.hpp.
struct response;
}  // namespace sba
namespace nef {
namespace app {

class nef_client;

class nef_app {
 public:
  explicit nef_app(
      const std::string& config_file, std::shared_ptr<nef_event>& ev);
  nef_app(nef_app const&) = delete;
  void operator=(nef_app const&) = delete;
  virtual ~nef_app();

  // Utility functions
  void generate_uuid();
  void generate_af_subscription_id(std::string& sub_id);

  void deregister_from_nrf();

  // Authorization
  bool authorize_af_request(
      const std::string& scs_as_id, const std::string& api_name) const;

  // Authorize an Nnef request whose URL path carries no AF identity: take
  // the 'sub' claim from the bearer token and hand that to
  // authorize_af_request(), which behaves as it would for an empty af_id and
  // falls back to insecure_dev_mode when nothing is configured.
  bool authorize_nnef_request(const std::string& api_name) const;

  // Per-request auth context set by HTTP layer before dispatch.
  void set_request_bearer_token(const std::string& bearer_token) const;
  void clear_request_bearer_token() const;
  [[nodiscard]] std::string get_request_bearer_token() const;

  // Monitoring Event Exposure (3GPP TS 29.122 §5.6)

  void handle_monitoring_event_subscription_get(
      const std::string& scs_as_id, const std::string& sub_id,
      nlohmann::json& response_body, int& http_code);

  // Monitoring Event UPDATE (PUT)
  void handle_monitoring_event_subscription_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

  // TI GET and LIST
  void handle_traffic_influence_get(
      const std::string& af_id, const std::string& app_session_id,
      nlohmann::json& response_body, int& http_code);
  void handle_traffic_influence_list(
      const std::string& af_id, nlohmann::json& response_body, int& http_code);

  // PFD transaction-level and app-level endpoints
  void handle_pfd_transaction_list(
      const std::string& scs_as_id, nlohmann::json& response_body,
      int& http_code);

  void handle_pfd_app_get(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, nlohmann::json& response_body, int& http_code);

  // Monitoring create
  void monitoring_event_subscribe(
      const std::string& scs_as_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_monitoring_event_subscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, oai::sba::response r, response_sink sink);

  // Qos create (TS 29.522 §4.4.9). The Location/self
  // header is produced by the adapter's qos-create header sink.
  void qos_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_qos_create(
      const std::string& af_id, const std::string& qos_sub_id,
      nlohmann::json req_data_json, oai::sba::response r, response_sink sink);

  // Traffic-influence create — a PCF call chained into a UDR call
  void ti_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_ti_create_pcf(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& ti_id, const std::string& pcf_ep,
      const std::string& udr_ep, std::shared_ptr<nef_subscription> ti_sub,
      oai::sba::response r, response_sink sink);
  void cont_ti_create_udr(
      const nlohmann::json& body, const std::string& ti_id,
      oai::sba::response r, response_sink sink);

  // Traffic-influence update
  void ti_update(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void cont_ti_update(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& body, oai::sba::response r, response_sink sink);

  // Traffic-influence patch
  void ti_patch(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& patch_body, const std::string& token,
      response_sink sink);
  void cont_ti_patch(
      const std::string& af_id, const std::string& ti_id,
      const std::string& app_session_id, nlohmann::json patched_copy,
      oai::sba::response r, response_sink sink);

  // PFD app put
  void pfd_app_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_pfd_app_put(
      const std::string& scs_as_id, const std::string& app_id,
      nlohmann::json new_app_json, bool is_create, oai::sba::response r,
      response_sink sink);

  // Monitoring + QoS deletes/updates
  // Monitoring delete
  void monitoring_event_unsubscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& token, response_sink sink);
  void cont_monitoring_event_unsubscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& nf_sub_id, oai::sba::response r, response_sink sink);

  // Qos update (PUT)
  void qos_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void cont_qos_update(
      const std::string& scs_as_id, const std::string& sub_id,
      nlohmann::json response_data, oai::sba::response r, response_sink sink);

  // Qos patch
  void qos_patch(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& patch_body, const std::string& token,
      response_sink sink);
  void cont_qos_patch(
      const std::string& scs_as_id, const std::string& sub_id,
      nlohmann::json patched, oai::sba::response r, response_sink sink);

  // Qos delete
  void qos_delete(
      const std::string& af_id, const std::string& qos_sub_id,
      const std::string& token, response_sink sink);
  void cont_qos_delete(
      const std::string& af_id, const std::string& qos_sub_id,
      const std::string& nf_sub_id, oai::sba::response r, response_sink sink);

  // Bdt create
  void bdt_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_bdt_create(
      const std::string& af_id, const std::string& bdt_id,
      oai::_3gpp::model::BdtPolicy bdt_policy, oai::sba::response r,
      response_sink sink);

  // Bdt update
  void bdt_update(
      const std::string& af_id, const std::string& bdt_id,
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void cont_bdt_update(
      const std::string& af_id, const std::string& bdt_id,
      oai::_3gpp::model::BdtPolicy bdt_policy, oai::sba::response r,
      response_sink sink);

  // Bdt patch
  void bdt_patch(
      const std::string& af_id, const std::string& bdt_policy_id,
      const nlohmann::json& patch_body, const std::string& token,
      response_sink sink);
  void cont_bdt_patch(
      const std::string& af_id, const std::string& bdt_policy_id,
      nlohmann::json patched_copy, oai::sba::response r, response_sink sink);

  // Bdt delete
  void bdt_delete(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& token, response_sink sink);
  void cont_bdt_delete(
      const std::string& af_id, const std::string& bdt_id, oai::sba::response r,
      response_sink sink);

  // PFD T8 single (UDR)
  // PFD create
  void pfd_create(
      const std::string& app_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_pfd_create(
      const std::string& app_id, nlohmann::json body, oai::sba::response r,
      response_sink sink);

  // PFD delete
  void pfd_delete(
      const std::string& app_id, const std::string& token, response_sink sink);
  void cont_pfd_delete(
      const std::string& app_id, oai::sba::response r, response_sink sink);

  // PFD app patch
  void pfd_app_patch(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& patch_body,
      const std::string& token, response_sink sink);
  void cont_pfd_app_patch(
      const std::string& scs_as_id, const std::string& app_id,
      nlohmann::json patched, oai::sba::response r, response_sink sink);

  // PFD app delete
  void pfd_app_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& token, response_sink sink);
  void cont_pfd_app_delete(
      const std::string& scs_as_id, const std::string& app_id,
      oai::sba::response r, response_sink sink);

  // Nnef-PFD single (UDR)
  // Nnef PFD put app
  void nnef_pfd_put_app(
      const std::string& transaction_id, const std::string& app_id,
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void cont_nnef_pfd_put_app(
      const std::string& app_id, nlohmann::json response_app,
      nlohmann::json normalized_app, bool is_create, oai::sba::response r,
      response_sink sink);

  // Nnef PFD delete app

  void nnef_pfd_delete_app(
      const std::string& transaction_id, const std::string& app_id,
      const std::string& token, response_sink sink);
  void cont_nnef_pfd_delete_app(
      const std::string& app_id, oai::sba::response r, response_sink sink);

  // PFD transaction delete — UDR DELETE
  struct PfdDeleteChain {
    std::string udr_ep;
    std::vector<std::string> app_ids;
    std::size_t idx = 0;
    response_sink sink;
  };
  void pfd_transaction_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& token, response_sink sink);
  void pfd_transaction_delete_step(std::shared_ptr<PfdDeleteChain> st);

  // nnef_pfd_delete_transaction
  void nnef_pfd_delete_transaction(
      const std::string& transaction_id, const std::string& token,
      response_sink sink);
  void nnef_pfd_delete_transaction_step(std::shared_ptr<PfdDeleteChain> st);

  // Nnef PFD partial pull- UDR GET
  struct PfdPullChain {
    std::string udr_ep;
    std::vector<std::pair<std::string, nlohmann::json>>
        apps;  // app_id, fallback
    std::size_t idx       = 0;
    nlohmann::json result = nlohmann::json::array();  // accumulated body
    response_sink sink;
  };
  void nnef_pfd_partial_pull(
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void nnef_pfd_partial_pull_step(std::shared_ptr<PfdPullChain> st);

  // Pfd_transaction_put
  struct PfdPutChain {
    std::string scs_as_id;
    std::string trans_id;
    bool is_create = false;
    std::string udr_ep;
    nlohmann::json body;  // echoed in the success response
    std::vector<std::pair<std::string, nlohmann::json>>
        apps;  // app_id, pfd json
    std::map<std::string, oai::_3gpp::model::PfdDataForApp>
        app_map;  // local commit
    std::size_t idx = 0;
    std::vector<std::string> committed;  // for reverse-order rollback
    response_sink sink;
  };
  void pfd_transaction_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void pfd_put_step(std::shared_ptr<PfdPutChain> st);
  void pfd_put_rollback(
      std::shared_ptr<PfdPutChain> st, const std::string& failed_app);
  void pfd_rollback_step(
      std::shared_ptr<PfdPutChain> st, std::size_t remaining,
      const std::string& failed_app);

  // Traffic influence delete — chained PCF→UDR (both deletes),
  void ti_delete(
      const std::string& af_id, const std::string& ti_id,
      const std::string& token, response_sink sink);
  void cont_ti_delete_pcf(
      const std::string& af_id, const std::string& ti_id,
      const std::string& udr_ep, oai::sba::response r, response_sink sink);
  void cont_ti_delete_udr(
      const std::string& af_id, const std::string& ti_id, oai::sba::response r,
      response_sink sink);

  // Nnef PFD put transaction
  struct NnefPutChain {
    std::string transaction_id;
    bool is_create = false;
    std::string udr_ep;
    nlohmann::json transaction;
    nlohmann::json applications;
    std::vector<std::string> app_ids;
    std::size_t idx = 0;
    std::vector<std::string> committed;
    std::vector<std::string> removed_apps;
    response_sink sink;
  };
  void nnef_pfd_put_transaction(
      const std::string& transaction_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void nnef_put_step(std::shared_ptr<NnefPutChain> st);
  void nnef_put_rollback(
      std::shared_ptr<NnefPutChain> st, const std::string& failed_app);
  void nnef_rollback_step(
      std::shared_ptr<NnefPutChain> st, std::size_t remaining,
      const std::string& failed_app);
  void nnef_put_after_commit(std::shared_ptr<NnefPutChain> st);

  // Nnef_PFDmanagement (TS 29.551)
  void handle_nnef_pfd_list_transactions(
      nlohmann::json& response_body, int& http_code);

  void handle_nnef_pfd_get_transaction(
      const std::string& transaction_id, nlohmann::json& response_body,
      int& http_code);

  void handle_nnef_pfd_get_app(
      const std::string& transaction_id, const std::string& app_id,
      nlohmann::json& response_body, int& http_code);

  // GET /nnef-pfdmanagement/v1/applications
  void handle_nnef_pfd_get_applications(
      const std::vector<std::string>& app_ids_filter,
      nlohmann::json& response_body, int& http_code);

  // PFD management subscription CRUD
  void handle_nnef_pfd_subscription_create(
      const nlohmann::json& body, std::string& sub_id,
      nlohmann::json& response_body, int& http_code);
  void handle_nnef_pfd_subscription_get(
      const std::string& sub_id, nlohmann::json& response_body, int& http_code);
  void handle_nnef_pfd_subscription_put(
      const std::string& sub_id, const nlohmann::json& body,
      nlohmann::json& response_body, int& http_code);
  void handle_nnef_pfd_subscription_delete(
      const std::string& sub_id, int& http_code);

  // Background Data Transfer (3GPP TS 29.122 §5.13)

  void handle_bdt_policy_list(
      const std::string& af_id, nlohmann::json& response_body, int& http_code);

  void handle_bdt_policy_get(
      const std::string& af_id, const std::string& bdt_id,
      nlohmann::json& response_body, int& http_code);

  // QoS Provisioning / Monitoring (3GPP TS 29.122 §5.7)

  void handle_qos_subscription_get(
      const std::string& af_id, const std::string& qos_sub_id,
      nlohmann::json& response_body, int& http_code);

  void handle_qos_subscription_list(
      const std::string& af_id, nlohmann::json& response_body, int& http_code);

  // Translate a T8 AsSessionWithQoSSubscription (TS 29.122) into a PCF
  // AppSessionContext{ascReqData} JSON body (TS 29.514) for
  // create_pcf_policy_auth.
  static bool build_pcf_qos_body(
      const oai::_3gpp::model::AsSessionWithQoSSubscription& req,
      const std::string& evsubsc_notif_uri, nlohmann::json& pcf_body,
      std::string& err);

  // Validate a PCF-returned appSessionId before storing / building URLs.
  // Rejects empty, '/', '\\', "..", whitespace, control chars, or length > 253.
  // (DELETE/PATCH concatenate it into ".../app-sessions/{id}" URLs.)
  static bool is_valid_app_session_id(const std::string& id);

  // Analytics (3GPP TS 29.520)
  void handle_analytics_subscription_create(
      const std::string& af_id, const nlohmann::json& body,
      std::string& analytics_sub_id, nlohmann::json& response_body,
      int& http_code);

  void handle_analytics_subscription_delete(
      const std::string& af_id, const std::string& analytics_sub_id,
      int& http_code);

  void handle_analytics_subscription_get(
      const std::string& af_id, const std::string& analytics_sub_id,
      nlohmann::json& response_body, int& http_code);

  void handle_analytics_subscription_list(
      const std::string& af_id, nlohmann::json& response_body, int& http_code);

  // Analytics UPDATE (PUT)
  void handle_analytics_subscription_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

  // Analytics /fetch endpoint
  void handle_analytics_fetch(
      const std::string& scs_as_id, const nlohmann::json& body,
      nlohmann::json& response_body, int& http_code);

  // Inbound notification from 5GC NF; returns false if nf_sub_id is unknown.
  bool handle_nf_notification(
      const std::string& nf_sub_id, const nlohmann::json& notif_payload);

  // Nnef_EventExposure (TS 29.591)
  void handle_nnef_event_exposure_subscribe(
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

  void handle_nnef_event_exposure_unsubscribe(
      const std::string& subscription_id, int& http_code);

  void handle_nnef_event_exposure_get(
      const std::string& subscription_id, nlohmann::json& response_body,
      int& http_code);

  void handle_nnef_event_exposure_update(
      const std::string& subscription_id, const nlohmann::json& body,
      nlohmann::json& response_body, int& http_code);

  // AF Profile management (mirrors nrf_app NF profile management)
  bool add_af_profile(
      const std::string& af_id, const std::shared_ptr<nef_af_profile>& p);

  bool remove_af_profile(const std::string& af_id);

  std::shared_ptr<nef_af_profile> find_af_profile(
      const std::string& af_id) const;

  bool is_af_registered(const std::string& af_id) const;

  /// Returns the NEF NF instance UUID (set during initialisation).
  std::string get_nef_instance_id() const { return m_nef_instance_id; }

 private:
  std::string m_nef_instance_id;

  // Concurrency contract for the state below. READ BEFORE REFACTORING.
  //
  // All ten mutexes are `mutable std::shared_mutex`. Each guards exactly
  // the maps declared beside it, and nothing else.
  //
  // THE INVARIANT: no code path ever holds two of these mutexes at the
  // same time.
  //
  // Every acquisition sits in its own `{ ... }` block and is released
  // before the next is taken, and no lock region calls another nef_app
  // member that locks a different mutex. This was measured over all 79
  // acquisition sites across the eight nef_app_*.cpp translation units
  // (39 lock_guard, 36 shared_lock, 4 unique_lock); it is not an
  // assumption.
  //
  // That invariant is the ONLY reason the lock order here is harmless,
  // because the order is NOT uniform. Four functions take two or more
  // distinct mutexes:
  //
  //   cont_qos_create                   qos -> nf2af
  //   cont_ti_create_pcf                ti  -> nf2af
  //   cont_ti_delete_udr                ti  -> nf2af
  //   handle_subscription_expiry_tick   nnef_event -> af_subscriptions
  //                                     -> nf2af -> ti      <-- REVERSED
  //
  // handle_subscription_expiry_tick takes m_nf2af_mutex and then
  // m_ti_mutex. That is the reverse of the three request-path functions
  // above it, which take m_ti_mutex or m_qos_mutex and then
  // m_nf2af_mutex. The expiry tick also runs on the task_manager tick
  // thread, not a request thread, so both orders really do execute
  // concurrently.
  //
  // The ABBA cycle is therefore already fully assembled; only block
  // scoping keeps the two halves from ever being held at once. In two
  // places the closing brace of one lock block and the opening of the
  // next are adjacent -- deleting that pair of braces is a one-character
  // edit that produces a real deadlock.
  //
  // So treat "never hold two" as a contract to preserve, not an accident
  // to lean on. Each of the following converts that latent ABBA into a
  // live deadlock, and none of them looks dangerous in review:
  //
  //   1. hoisting a guard out of its block up to function scope;
  //   2. merging two adjacent lock blocks into one;
  //   3. moving a map behind a helper or service object that locks
  //      internally, so an outer lock region now calls into it;
  //   4. letting a sink, callback or continuation re-enter nef_app while
  //      a store lock is held. 33 sink(...) calls already run under a
  //      held store mutex across 16 methods; they are safe only because
  //      every one of them is handed a deferred sink, which hands off to
  //      the libevent thread and never touches a nef_app mutex.
  //
  // If two mutexes ever genuinely must be held together, stop and define
  // a documented lock order first, then make the expiry tick conform to
  // it. Do not add the second acquisition and hope.
  //
  // Unrelated to lock order, but in the same blast radius: nef_subscription
  // itself has no mutex. shared_ptrs to it are handed out from under
  // m_af_subscriptions_mutex and then mutated with no lock held.
  //
  // AF subscriptions map (af_sub_id → subscription)
  std::map<std::string, std::shared_ptr<nef_subscription>>
      m_af_sub_id2subscription;
  mutable std::shared_mutex m_af_subscriptions_mutex;

  // NF→AF sub-id mapping  (nf_sub_id → af_sub_id)
  std::map<std::string, std::string> m_nf2af_sub_id;
  mutable std::shared_mutex m_nf2af_mutex;

  // Traffic influence sessions (ti_id → body)
  std::map<std::string, nlohmann::json> m_ti_sessions;
  std::map<std::string, std::string> m_ti_id2af_id;
  std::map<std::string, std::string> m_ti_id2pcf_policy_id;
  mutable std::shared_mutex m_ti_mutex;

  // QoS AF-session → PCF appSessionId. Distinct from TI's m_ti_id2pcf_policy_id
  // Keyed by the NEF qos_sub_id; value is the PCF-returned appSessionId.
  // Guarded by m_qos_mutex.
  std::map<std::string, std::string> m_qos_sub_id2pcf_app_session_id;
  mutable std::shared_mutex m_qos_mutex;

  // BDT policy sessions (bdt_id → typed BdtPolicy)
  std::map<std::string, oai::_3gpp::model::BdtPolicy> m_bdt_sessions;
  std::map<std::string, std::string> m_bdt_id2af_id;
  std::map<std::string, std::string> m_bdt_id2pcf_policy_id;
  mutable std::shared_mutex m_bdt_mutex;

  // PFD transactions (trans_id → typed app map)
  std::map<std::string, std::map<std::string, oai::_3gpp::model::PfdDataForApp>>
      m_pfd_trans_sessions;
  std::map<std::string, std::string> m_pfd_trans2scs_id;
  mutable std::shared_mutex m_pfd_mutex;

  // SBI PFD management storage (distinct from T8 PFD storage)
  std::unordered_map<std::string, nlohmann::json> m_nnef_pfd_transactions;
  mutable std::shared_mutex m_nnef_pfd_transactions_mutex;

  // F3.2: Nnef_PFDmanagement subscriptions (sub_id → typed PfdSubscription)
  std::unordered_map<std::string, oai::_3gpp::model::PfdSubscription>
      m_nnef_pfd_subscriptions;
  mutable std::shared_mutex m_nnef_pfd_subscriptions_mutex;

  // Nnef_EventExposure subscriptions (subscription_id → typed
  // NefEventExposureSubsc)
  std::unordered_map<std::string, oai::_3gpp::model::NefEventExposureSubsc>
      m_nnef_event_subscriptions;
  mutable std::shared_mutex m_nnef_event_subscriptions_mutex;

  std::shared_ptr<nef_event> m_event_sub;
  std::vector<bs2::connection> m_connections;
  oai::utils::uint_generator<uint32_t> m_sub_id_generator;

  std::shared_ptr<nef_client> m_nef_client;
  std::unique_ptr<notification_thread_pool> m_notification_pool;
  std::atomic<bool> m_deregistered{false};

  // AF profile store: af_id → profile
  std::map<std::string, std::shared_ptr<nef_af_profile>> m_af_id2profile;
  mutable std::shared_mutex m_af_id2profile_mutex;

  // Internal helpers

  // The "AF/NF not authorized" rejection, in the four spellings the call
  // sites need. Each returns true when the caller must stop; false means the
  // request is authorized and nothing was written.
  //
  // The response_sink overloads clear the bearer token BEFORE they answer.
  // That ORDERING -- not the mere presence of the clear -- is the invariant
  // the async path depends on; see the thread_local bearer-token banner at
  // the top of nef_app_core.cpp. Folding it in here makes it one place to
  // audit instead of seventeen.
  //
  // AF and NF stay apart on purpose. They are different predicates
  // (authorize_af_request vs authorize_nnef_request) AND different detail
  // strings, so one shared helper would change what the NF responses say.
  bool reject_unauthorized_af(
      const std::string& af_id, const std::string& api_name,
      nlohmann::json& response_body, int& http_code) const;
  bool reject_unauthorized_af(
      const std::string& af_id, const std::string& api_name,
      const response_sink& sink) const;
  bool reject_unauthorized_nf(
      const std::string& api_name, nlohmann::json& response_body,
      int& http_code) const;
  bool reject_unauthorized_nf(
      const std::string& api_name, const response_sink& sink) const;

  bool add_subscription(
      const std::string& sub_id, const std::shared_ptr<nef_subscription>& s);
  bool remove_subscription(const std::string& sub_id);
  std::shared_ptr<nef_subscription> find_subscription(
      const std::string& sub_id) const;
  bool is_subscription_owner(
      const std::shared_ptr<nef_subscription>& sub,
      const std::string& af_id) const;

  void subscribe_nf_notification();
  void handle_nf_notification_event(
      const std::string& nf_sub_id, const nlohmann::json& notif);

  void handle_subscription_expiry_tick(uint64_t t);

  // Dispatch PFD-change notifications to all matching SBI subscribers.
  void notify_nnef_pfd_subscribers(
      const std::string& event_type,  // "PFD_CHANGE" or "PFD_REMOVE"
      const std::string& app_id, const nlohmann::json& pfd_data);

  // Lifecycle helpers: create profile on first subscription, destroy on last
  void ensure_af_profile(const std::string& af_id, const std::string& sub_id);
  void release_af_profile_subscription(
      const std::string& af_id, const std::string& sub_id);
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_APP_HPP_SEEN */
