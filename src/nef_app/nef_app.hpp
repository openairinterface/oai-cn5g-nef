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

#include "BdtPolicy.h"
#include "NefEventExposureSubsc.h"
#include "PfdDataForApp.h"
#include "PfdSubscription.h"
#include "nef.h"
#include "nef_af_profile.hpp"
#include "nef_event.hpp"
#include "nef_notification_queue.hpp"
#include "nef_subscription.hpp"
#include "uint_generator.hpp"

namespace bs2 = boost::signals2;

namespace oai {
namespace nef {
namespace app {

class nef_client;

class nef_app {
 public:
  explicit nef_app(const std::string& config_file, nef_event& ev);
  nef_app(nef_app const&) = delete;
  void operator=(nef_app const&) = delete;
  virtual ~nef_app();

  // Utility functions
  void generate_uuid();
  void generate_af_subscription_id(std::string& sub_id);

  // Lifecycle
  // Explicitly deregister from the NRF. Safe to call more than once — the
  // second and subsequent calls are no-ops (double-deregistration guard).
  void deregister_from_nrf();

  // Authorization
  bool authorize_af_request(
      const std::string& scs_as_id, const std::string& api_name) const;

  // Authorize an Nnef request where the AF identity is NOT in the URL path.
  // Extracts the 'sub' claim from the bearer token and delegates to
  // authorize_af_request().  Falls back to insecure_dev_mode when no auth is
  // configured (same semantics as authorize_af_request with an empty af_id).
  bool authorize_nnef_request(const std::string& api_name) const;

  // Per-request auth context set by HTTP layer before dispatch.
  void set_request_bearer_token(const std::string& bearer_token) const;
  void clear_request_bearer_token() const;

  // Monitoring Event Exposure (3GPP TS 29.122 §5.6)
  void handle_monitoring_event_subscription_create(
      const std::string& scs_as_id, const nlohmann::json& body,
      std::string& sub_id, nlohmann::json& response_body, int& http_code);

  void handle_monitoring_event_subscription_delete(
      const std::string& scs_as_id, const std::string& sub_id, int& http_code);

  void handle_monitoring_event_subscription_get(
      const std::string& scs_as_id, const std::string& sub_id,
      nlohmann::json& response_body, int& http_code);

  // Monitoring Event UPDATE (PUT)
  void handle_monitoring_event_subscription_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

  // Traffic Influence (3GPP TS 29.522 §5.3)
  void handle_traffic_influence_create(
      const std::string& af_id, const nlohmann::json& body, std::string& ti_id,
      nlohmann::json& response_body, int& http_code);

  void handle_traffic_influence_update(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

  void handle_traffic_influence_delete(
      const std::string& af_id, const std::string& ti_id, int& http_code);

  // TI GET and LIST
  void handle_traffic_influence_get(
      const std::string& af_id, const std::string& app_session_id,
      nlohmann::json& response_body, int& http_code);
  void handle_traffic_influence_list(
      const std::string& af_id, nlohmann::json& response_body, int& http_code);

  // TI PATCH
  void handle_traffic_influence_patch(
      const std::string& af_id, const std::string& app_session_id,
      const nlohmann::json& patch_body, nlohmann::json& response_body,
      int& http_code);

  // PFD Management (3GPP TS 29.122 §5.12)
  void handle_pfd_create(
      const std::string& app_id, const nlohmann::json& body,
      nlohmann::json& response_body, int& http_code);

  void handle_pfd_delete(const std::string& app_id, int& http_code);

  void handle_pfd_get(
      const std::string& app_id, nlohmann::json& response_body, int& http_code);

  // PFD transaction-level and app-level endpoints
  void handle_pfd_transaction_list(
      const std::string& scs_as_id, nlohmann::json& response_body,
      int& http_code);

  void handle_pfd_transaction_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

  void handle_pfd_transaction_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      int& http_code);

  void handle_pfd_app_get(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, nlohmann::json& response_body, int& http_code);

  void handle_pfd_app_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& body,
      nlohmann::json& response_body, int& http_code);

  void handle_pfd_app_patch(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& patch_body,
      nlohmann::json& response_body, int& http_code);

  void handle_pfd_app_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, int& http_code);

  // Nnef_PFDmanagement (TS 29.551)
  void handle_nnef_pfd_list_transactions(
      nlohmann::json& response_body, int& http_code);

  void handle_nnef_pfd_put_transaction(
      const std::string& transaction_id, const nlohmann::json& body,
      nlohmann::json& response_body, int& http_code);

  void handle_nnef_pfd_get_transaction(
      const std::string& transaction_id, nlohmann::json& response_body,
      int& http_code);

  void handle_nnef_pfd_delete_transaction(
      const std::string& transaction_id, int& http_code);

  void handle_nnef_pfd_get_app(
      const std::string& transaction_id, const std::string& app_id,
      nlohmann::json& response_body, int& http_code);

  void handle_nnef_pfd_put_app(
      const std::string& transaction_id, const std::string& app_id,
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

  void handle_nnef_pfd_delete_app(
      const std::string& transaction_id, const std::string& app_id,
      int& http_code);

  // GET /nnef-pfdmanagement/v1/applications
  void handle_nnef_pfd_get_applications(
      const std::vector<std::string>& app_ids_filter,
      nlohmann::json& response_body, int& http_code);

  // POST /nnef-pfdmanagement/v1/applications/partial-pull
  void handle_nnef_pfd_partial_pull(
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

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
  void handle_bdt_policy_create(
      const std::string& af_id, const nlohmann::json& body, std::string& bdt_id,
      nlohmann::json& response_body, int& http_code);

  void handle_bdt_policy_update(
      const std::string& af_id, const std::string& bdt_id,
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

  void handle_bdt_policy_delete(
      const std::string& af_id, const std::string& bdt_id, int& http_code);

  void handle_bdt_policy_list(
      const std::string& af_id, nlohmann::json& response_body, int& http_code);

  void handle_bdt_policy_get(
      const std::string& af_id, const std::string& bdt_id,
      nlohmann::json& response_body, int& http_code);

  // BDT PATCH
  void handle_bdt_policy_patch(
      const std::string& af_id, const std::string& bdt_policy_id,
      const nlohmann::json& patch_body, nlohmann::json& response_body,
      int& http_code);

  // QoS Provisioning / Monitoring (3GPP TS 29.122 §5.7)
  void handle_qos_subscription_create(
      const std::string& af_id, const nlohmann::json& body,
      std::string& qos_sub_id, nlohmann::json& response_body, int& http_code);

  void handle_qos_subscription_delete(
      const std::string& af_id, const std::string& qos_sub_id, int& http_code);

  void handle_qos_subscription_get(
      const std::string& af_id, const std::string& qos_sub_id,
      nlohmann::json& response_body, int& http_code);

  void handle_qos_subscription_list(
      const std::string& af_id, nlohmann::json& response_body, int& http_code);

  // QoS UPDATE (PUT)
  void handle_qos_subscription_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, nlohmann::json& response_body,
      int& http_code);

  // QoS PATCH
  void handle_qos_subscription_patch(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& patch_body, nlohmann::json& response_body,
      int& http_code);

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

  // Inbound notification from 5GC NF
  void handle_nf_notification(
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

  nef_event& m_event_sub;
  std::vector<bs2::connection> m_connections;
  oai::utils::uint_generator<uint32_t> m_sub_id_generator;

  std::shared_ptr<nef_client> m_nef_client;
  std::unique_ptr<notification_thread_pool> m_notification_pool;
  std::atomic<bool> m_deregistered{false};

  // AF profile store: af_id → profile
  std::map<std::string, std::shared_ptr<nef_af_profile>> m_af_id2profile;
  mutable std::shared_mutex m_af_id2profile_mutex;

  // Internal helpers
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
