/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_HTTP2_SERVER_SEEN
#define FILE_NEF_HTTP2_SERVER_SEEN

#include <atomic>
#include <chrono>
#include <memory>

#include "http2_server.h"
#include "nef_app.hpp"
#include "uint_generator.hpp"

#include "nef_app_adapter.hpp"

namespace oai::nef::api {

class nef_http2_server {
 public:
  nef_http2_server(
      std::string addr, uint32_t port,
      const std::shared_ptr<oai::nef::app::nef_app>& nef_app_inst,
      http2_server_config config = {})
      : m_address(addr),
        m_port(port),
        server_(addr, port, config),
        m_nef_app(nef_app_inst),
        m_start_time(std::chrono::steady_clock::now()) {}

  void start();
  void stop();
  /// Signal the server to enter drain mode: new requests receive 503.
  void initiate_graceful_shutdown();

  // Monitoring Event
  void handle_monitoring_event_subscribe(
      const std::string& scs_as_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);

  void handle_monitoring_event_unsubscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& bearer_token, http2_response& response);

  void handle_monitoring_event_get(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& bearer_token, http2_response& response);

  // Monitoring Event UPDATE (PUT)
  void handle_monitoring_event_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& body, const std::string& bearer_token,
      http2_response& response);

  // Nnef_EventExposure (TS 29.591)
  void handle_nnef_event_exposure_subscribe(
      const std::string& body, const std::string& bearer_token,
      http2_response& response);

  void handle_nnef_event_exposure_unsubscribe(
      const std::string& subscription_id, const std::string& bearer_token,
      http2_response& response);

  void handle_nnef_event_exposure_get(
      const std::string& subscription_id, const std::string& bearer_token,
      http2_response& response);

  void handle_nnef_event_exposure_update(
      const std::string& subscription_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);

  // Traffic Influence
  void handle_ti_create(
      const std::string& af_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);

  // TI GET and LIST
  void handle_ti_get(
      const std::string& af_id, const std::string& ti_id,
      const std::string& bearer_token, http2_response& response);
  void handle_ti_list(
      const std::string& af_id, const std::string& bearer_token,
      http2_response& response);

  void handle_ti_update(
      const std::string& af_id, const std::string& ti_id,
      const std::string& body, const std::string& bearer_token,
      http2_response& response);

  void handle_ti_delete(
      const std::string& af_id, const std::string& ti_id,
      const std::string& bearer_token, http2_response& response);

  // TI PATCH
  void handle_ti_patch(
      const std::string& af_id, const std::string& ti_id,
      const std::string& patch_body, const std::string& bearer_token,
      http2_response& response);

  // PFD transaction-level and app-level handlers
  void handle_pfd_transaction_list(
      const std::string& scs_as_id, const std::string& bearer_token,
      http2_response& response);

  void handle_pfd_transaction_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& body, const std::string& bearer_token,
      http2_response& response);

  void handle_pfd_transaction_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& bearer_token, http2_response& response);

  void handle_pfd_app_get(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& bearer_token,
      http2_response& response);

  void handle_pfd_app_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);

  void handle_pfd_app_patch(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& patch_body,
      const std::string& bearer_token, http2_response& response);

  void handle_pfd_app_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& bearer_token,
      http2_response& response);

  // Nnef_PFDmanagement (TS 29.551)
  void handle_nnef_pfd_list_transactions(
      const std::string& bearer_token, http2_response& response);

  void handle_nnef_pfd_put_transaction(
      const std::string& trans_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);

  void handle_nnef_pfd_get_transaction(
      const std::string& trans_id, const std::string& bearer_token,
      http2_response& response);

  void handle_nnef_pfd_delete_transaction(
      const std::string& trans_id, const std::string& bearer_token,
      http2_response& response);

  void handle_nnef_pfd_get_app(
      const std::string& trans_id, const std::string& app_id,
      const std::string& bearer_token, http2_response& response);

  void handle_nnef_pfd_put_app(
      const std::string& trans_id, const std::string& app_id,
      const std::string& body, const std::string& bearer_token,
      http2_response& response);

  void handle_nnef_pfd_delete_app(
      const std::string& trans_id, const std::string& app_id,
      const std::string& bearer_token, http2_response& response);

  // GET /applications and POST /applications/partial-pull
  void handle_nnef_pfd_get_applications(
      const std::vector<std::string>& app_ids_filter,
      const std::string& bearer_token, http2_response& response);
  void handle_nnef_pfd_partial_pull(
      const std::string& body, const std::string& bearer_token,
      http2_response& response);

  // PFD subscription CRUD
  void handle_nnef_pfd_subscription_create(
      const std::string& body, const std::string& bearer_token,
      http2_response& response);
  void handle_nnef_pfd_subscription_get(
      const std::string& sub_id, const std::string& bearer_token,
      http2_response& response);
  void handle_nnef_pfd_subscription_put(
      const std::string& sub_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);
  void handle_nnef_pfd_subscription_delete(
      const std::string& sub_id, const std::string& bearer_token,
      http2_response& response);

  // BDT Policy.
  // deprecated=true adds the x-deprecated:true response header, marking a
  // request that arrived on the legacy /policies path.
  void handle_bdt_create(
      const std::string& af_id, const std::string& body,
      const std::string& bearer_token, http2_response& response,
      bool deprecated = false);

  void handle_bdt_update(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& body, const std::string& bearer_token,
      http2_response& response, bool deprecated = false);

  void handle_bdt_delete(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& bearer_token, http2_response& response,
      bool deprecated = false);

  void handle_bdt_get(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& bearer_token, http2_response& response,
      bool deprecated = false);

  // BDT PATCH
  void handle_bdt_patch(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& patch_body, const std::string& bearer_token,
      http2_response& response);

  // QoS Monitoring
  void handle_qos_create(
      const std::string& af_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);

  void handle_qos_delete(
      const std::string& af_id, const std::string& sub_id,
      const std::string& bearer_token, http2_response& response);

  void handle_qos_get(
      const std::string& af_id, const std::string& sub_id,
      const std::string& bearer_token, http2_response& response);

  // QoS UPDATE (PUT)
  void handle_qos_update(
      const std::string& af_id, const std::string& sub_id,
      const std::string& body, const std::string& bearer_token,
      http2_response& response);

  // QoS PATCH
  void handle_qos_patch(
      const std::string& af_id, const std::string& sub_id,
      const std::string& patch_body, const std::string& bearer_token,
      http2_response& response);

  // Analytics
  void handle_analytics_create(
      const std::string& af_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);

  void handle_analytics_delete(
      const std::string& af_id, const std::string& sub_id,
      const std::string& bearer_token, http2_response& response);

  void handle_analytics_get(
      const std::string& af_id, const std::string& sub_id,
      const std::string& bearer_token, http2_response& response);

  // Analytics /fetch endpoint
  void handle_analytics_fetch(
      const std::string& af_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);

  // Analytics UPDATE (PUT)
  void handle_analytics_update(
      const std::string& af_id, const std::string& sub_id,
      const std::string& body, const std::string& bearer_token,
      http2_response& response);

  // Inbound NF notification (AMF/SMF/PCF → NEF → AF)
  /**
   * Called when a southbound NF POSTs a notification. Accepted paths:
   *   POST /nef-notify/v1/notify/{nf_sub_id}
   *   POST /nef-notify/v1/notify/amf   (legacy flat-path form)
   *   POST /nef-notify/v1/notify/smf
   *
   * The tail after /notify/ is passed on as nf_sub_id; the app layer maps it
   * to the AF subscription it belongs to.
   */
  void handle_nf_notify(
      const std::string& nf_sub_id, const std::string& body,
      const std::string& bearer_token, http2_response& response);

 private:
  // Routing
  // One member per path prefix registered in start(); start() itself is
  // nothing but the registration table.
  //
  // These are listed in registration order, and that order is part of the
  // contract — see the comment on the table in start().
  //
  // Each member owns its own path parsing. The twelve routes do NOT parse
  // alike: three different prefix-strip strategies, and different segment
  // indices read out of the split. No parsing helper is shared between them;
  // only begin_request() is.
  void route_nnef_event_exposure(const http2_request& req, http2_response& res);
  void route_monitoring_event(const http2_request& req, http2_response& res);
  void route_traffic_influence(const http2_request& req, http2_response& res);
  void route_pfd_management(const http2_request& req, http2_response& res);
  void route_nnef_pfd_transactions(
      const http2_request& req, http2_response& res);
  void route_nnef_pfd_applications(
      const http2_request& req, http2_response& res);
  void route_nnef_pfd_subscriptions(
      const http2_request& req, http2_response& res);
  void route_bdt(const http2_request& req, http2_response& res);
  void route_qos_monitoring(const http2_request& req, http2_response& res);
  void route_analytics(const http2_request& req, http2_response& res);
  void route_nf_notify(const http2_request& req, http2_response& res);
  void route_health(const http2_request& req, http2_response& res);

  /// Extracts the bearer token, then applies the drain (503) and rate-limit
  /// (429) guards, in that order.
  ///
  /// Returns false once a guard has answered the request; the caller must
  /// then return immediately. route_health() deliberately does not use this.
  bool begin_request(
      const http2_request& req, http2_response& res, std::string& bearer_token);

  /// Fills the nine m_*_base path members from the configured API version.
  /// Called once at the top of start(), before any request is admitted.
  void build_api_base_paths();

  /// Builds m_adapter. Called once by start(): after the routes are
  /// registered, before server_.start() admits any request.
  void construct_dispatch_adapter();

  std::string m_address;
  uint32_t m_port;

  // API base paths. Written once by build_api_base_paths() at the top of
  // start(), then read-only for the rest of the serving lifetime.
  //
  // Declared before server_ so they outlive it during destruction.
  std::string m_nnef_event_exposure_base;
  std::string m_nef_monitoring_event_base;
  std::string m_nef_traffic_influence_base;
  std::string m_nef_pfd_management_base;
  std::string m_nnef_pfd_management_base;
  std::string m_nef_bdt_base;
  std::string m_nef_qos_monitoring_base;
  std::string m_nef_analytics_base;
  std::string m_nef_notify_base;

  http2_server server_;
  std::shared_ptr<oai::nef::app::nef_app> m_nef_app;
  std::unique_ptr<oai::nef::app::nef_app_adapter> m_adapter;
  std::atomic<bool> m_draining{false};
  std::chrono::steady_clock::time_point m_start_time;

  oai::utils::uint_generator<uint32_t> m_promise_id_generator;
};

}  // namespace oai::nef::api

#endif /* FILE_NEF_HTTP2_SERVER_SEEN */
