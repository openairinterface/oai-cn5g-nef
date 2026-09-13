/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
#pragma once
#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "nef_request_dispatcher.hpp"
#include "nef_request_task.hpp"

// Forward declaration
class http2_deferred_response;
namespace oai::nef::app {
class nef_app;
}

namespace oai::nef::app {

// Typed dispatch facade between the HTTP/2 server and nef_app.
//
// This is the only class with direct nef_app access from the request path.
// nef-http2-server may still keep a nef_app* for non-request health/metadata
// uses, but route handlers must dispatch through this adapter rather than
// calling nef_app handlers or token APIs directly.
//
// Every dispatch_* call enqueues its execute_* work on the dispatcher worker
// pool (non-blocking; may return queue_full/stopped). The bearer-token
// set/call/clear discipline is performed exactly once in execute_with_token(),
// so token state does not leak across dispatcher tasks.
class nef_app_adapter {
 public:
  using dispatch_status = nef_request_dispatcher::dispatch_status;

  // Shares ownership of app, so the adapter can never outlive it.
  nef_app_adapter(
      const std::shared_ptr<nef_app>& app, std::size_t n_threads,
      std::size_t http_worker_count, std::size_t max_queue = 10000);
  ~nef_app_adapter() { stop(); }

  nef_app_adapter(const nef_app_adapter&) = delete;
  nef_app_adapter& operator=(const nef_app_adapter&) = delete;

  void stop();
  std::size_t queue_depth() const;

  // ── One dispatch_* method per nef_http2_server::handle_* method ──────────
  // Each takes only HTTP-derived params + bearer token (by value) + a
  // response_sink (by value). JSON bodies are passed already-parsed.

  // Traffic Influence
  dispatch_status dispatch_ti_get(
      const std::string& af_id, const std::string& ti_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_ti_list(
      const std::string& af_id, std::string token, response_sink sink);
  dispatch_status dispatch_ti_create(
      const std::string& af_id, const nlohmann::json& body, std::string token,
      response_sink sink);
  dispatch_status dispatch_ti_update(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& body, std::string token, response_sink sink);
  dispatch_status dispatch_ti_delete(
      const std::string& af_id, const std::string& ti_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_ti_patch(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& patch_body, std::string token, response_sink sink);

  // Monitoring Event
  dispatch_status dispatch_monitoring_event_subscribe(
      const std::string& scs_as_id, const nlohmann::json& body,
      std::string token, response_sink sink);
  dispatch_status dispatch_monitoring_event_unsubscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      std::string token, response_sink sink);
  dispatch_status dispatch_monitoring_event_get(
      const std::string& scs_as_id, const std::string& sub_id,
      std::string token, response_sink sink);
  dispatch_status dispatch_monitoring_event_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, std::string token, response_sink sink);

  // QoS
  dispatch_status dispatch_qos_create(
      const std::string& af_id, const nlohmann::json& body, std::string token,
      response_sink sink);
  dispatch_status dispatch_qos_delete(
      const std::string& af_id, const std::string& sub_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_qos_get(
      const std::string& af_id, const std::string& sub_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_qos_update(
      const std::string& af_id, const std::string& sub_id,
      const nlohmann::json& body, std::string token, response_sink sink);
  dispatch_status dispatch_qos_patch(
      const std::string& af_id, const std::string& sub_id,
      const nlohmann::json& patch_body, std::string token, response_sink sink);

  // BDT
  dispatch_status dispatch_bdt_create(
      const std::string& af_id, const nlohmann::json& body, std::string token,
      response_sink sink);
  dispatch_status dispatch_bdt_update(
      const std::string& af_id, const std::string& bdt_id,
      const nlohmann::json& body, std::string token, response_sink sink);
  dispatch_status dispatch_bdt_delete(
      const std::string& af_id, const std::string& bdt_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_bdt_get(
      const std::string& af_id, const std::string& bdt_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_bdt_patch(
      const std::string& af_id, const std::string& bdt_id,
      const nlohmann::json& patch_body, std::string token, response_sink sink);

  // Analytics
  dispatch_status dispatch_analytics_create(
      const std::string& af_id, const nlohmann::json& body, std::string token,
      response_sink sink);
  dispatch_status dispatch_analytics_delete(
      const std::string& af_id, const std::string& sub_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_analytics_get(
      const std::string& af_id, const std::string& sub_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_analytics_update(
      const std::string& af_id, const std::string& sub_id,
      const nlohmann::json& body, std::string token, response_sink sink);
  dispatch_status dispatch_analytics_fetch(
      const std::string& af_id, const nlohmann::json& body, std::string token,
      response_sink sink);

  // PFD (T8) transaction-level and app-level
  dispatch_status dispatch_pfd_create(
      const std::string& app_id, const nlohmann::json& body, std::string token,
      response_sink sink);
  dispatch_status dispatch_pfd_delete(
      const std::string& app_id, std::string token, response_sink sink);
  dispatch_status dispatch_pfd_transaction_list(
      const std::string& scs_as_id, std::string token, response_sink sink);
  dispatch_status dispatch_pfd_transaction_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const nlohmann::json& body, std::string token, response_sink sink);
  dispatch_status dispatch_pfd_transaction_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      std::string token, response_sink sink);
  dispatch_status dispatch_pfd_app_get(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, std::string token, response_sink sink);
  dispatch_status dispatch_pfd_app_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& body, std::string token,
      response_sink sink);
  dispatch_status dispatch_pfd_app_patch(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& patch_body,
      std::string token, response_sink sink);
  dispatch_status dispatch_pfd_app_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, std::string token, response_sink sink);

  // Nnef_PFDmanagement (TS 29.551)
  dispatch_status dispatch_nnef_pfd_list_transactions(
      std::string token, response_sink sink);
  dispatch_status dispatch_nnef_pfd_put_transaction(
      const std::string& trans_id, const nlohmann::json& body,
      std::string token, response_sink sink);
  dispatch_status dispatch_nnef_pfd_get_transaction(
      const std::string& trans_id, std::string token, response_sink sink);
  dispatch_status dispatch_nnef_pfd_delete_transaction(
      const std::string& trans_id, std::string token, response_sink sink);
  dispatch_status dispatch_nnef_pfd_get_app(
      const std::string& trans_id, const std::string& app_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_nnef_pfd_put_app(
      const std::string& trans_id, const std::string& app_id,
      const nlohmann::json& body, std::string token, response_sink sink);
  dispatch_status dispatch_nnef_pfd_delete_app(
      const std::string& trans_id, const std::string& app_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_nnef_pfd_get_applications(
      const std::vector<std::string>& app_ids_filter, std::string token,
      response_sink sink);
  dispatch_status dispatch_nnef_pfd_partial_pull(
      const nlohmann::json& body, std::string token, response_sink sink);
  dispatch_status dispatch_nnef_pfd_subscription_create(
      const nlohmann::json& body, std::string token, response_sink sink);
  dispatch_status dispatch_nnef_pfd_subscription_get(
      const std::string& sub_id, std::string token, response_sink sink);
  dispatch_status dispatch_nnef_pfd_subscription_put(
      const std::string& sub_id, const nlohmann::json& body, std::string token,
      response_sink sink);
  dispatch_status dispatch_nnef_pfd_subscription_delete(
      const std::string& sub_id, std::string token, response_sink sink);

  // Nnef_EventExposure (TS 29.591)
  dispatch_status dispatch_nnef_event_exposure_subscribe(
      const nlohmann::json& body, std::string token, response_sink sink);
  dispatch_status dispatch_nnef_event_exposure_unsubscribe(
      const std::string& subscription_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_nnef_event_exposure_get(
      const std::string& subscription_id, std::string token,
      response_sink sink);
  dispatch_status dispatch_nnef_event_exposure_update(
      const std::string& subscription_id, const nlohmann::json& body,
      std::string token, response_sink sink);

  // Inbound NF notification — bool return; sink encodes 404 vs 204 internally.
  dispatch_status dispatch_nf_notification(
      const std::string& nf_sub_id, const nlohmann::json& body,
      std::string token, response_sink sink);

  // Async variants: Each enqueues its existing execute_* on the dispatcher
  // worker pool and delivers the result through the moved-in
  // http2_deferred_response (which posts the response back onto the libevent
  // loop). The HTTP worker thread returns immediately after dispatch — no
  // fut.wait(). Returns false if the dispatch was rejected
  // (queue_full/stopped); the adapter sends an explicit 503 through the
  // deferred handle in that case so clients do not receive the deferred
  // handle's generic fallback 500.
  //
  // NOTE: these run the existing synchronous nef_app handler on a dispatcher
  // worker; the southbound HTTP call inside nef_app is still blocking for that
  // worker. Making the southbound hop itself non-blocking (via the async
  // nef_client variants) additionally requires splitting the monolithic
  // nef_app handlers into pre-/post-southbound phases, which is a separate
  // follow-up.
  bool dispatch_monitoring_event_subscribe_async(
      const std::string& scs_as_id, const nlohmann::json& body,
      std::string token, http2_deferred_response deferred);
  bool dispatch_qos_create_async(
      const std::string& af_id, const nlohmann::json& body, std::string token,
      const std::string& server_address, http2_deferred_response deferred);
  bool dispatch_ti_create_async(
      const std::string& af_id, const nlohmann::json& body, std::string token,
      http2_deferred_response deferred);
  bool dispatch_ti_update_async(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& body, std::string token,
      http2_deferred_response deferred);
  bool dispatch_ti_patch_async(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& patch_body, std::string token,
      http2_deferred_response deferred);
  bool dispatch_pfd_app_put_async(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& body, std::string token,
      http2_deferred_response deferred);

  // Async dispatch
  // Same contract as the dispatch_*_async above: build the matching sink,
  // enqueue the entry method on the dispatcher worker, and answer 503 if the
  // dispatcher refuses the work.
  bool dispatch_monitoring_event_unsubscribe_async(
      const std::string& scs_as_id, const std::string& sub_id,
      std::string token, http2_deferred_response deferred);
  bool dispatch_qos_update_async(
      const std::string& af_id, const std::string& sub_id,
      const nlohmann::json& body, std::string token,
      http2_deferred_response deferred);
  bool dispatch_qos_patch_async(
      const std::string& af_id, const std::string& sub_id,
      const nlohmann::json& patch_body, std::string token,
      http2_deferred_response deferred);
  bool dispatch_qos_delete_async(
      const std::string& af_id, const std::string& sub_id, std::string token,
      http2_deferred_response deferred);
  // BDT — `deprecated` reproduces the x-deprecated legacy-path header
  // the sync shims emit (header sink for create/update, header-carrying empty
  // sink for delete).
  bool dispatch_bdt_create_async(
      const std::string& af_id, const nlohmann::json& body, std::string token,
      bool deprecated, http2_deferred_response deferred);
  bool dispatch_bdt_update_async(
      const std::string& af_id, const std::string& bdt_id,
      const nlohmann::json& body, std::string token, bool deprecated,
      http2_deferred_response deferred);
  bool dispatch_bdt_patch_async(
      const std::string& af_id, const std::string& bdt_id,
      const nlohmann::json& patch_body, std::string token,
      http2_deferred_response deferred);
  bool dispatch_bdt_delete_async(
      const std::string& af_id, const std::string& bdt_id, std::string token,
      bool deprecated, http2_deferred_response deferred);
  // PFD T8
  bool dispatch_pfd_create_async(
      const std::string& app_id, const nlohmann::json& body, std::string token,
      http2_deferred_response deferred);
  bool dispatch_pfd_delete_async(
      const std::string& app_id, std::string token,
      http2_deferred_response deferred);
  bool dispatch_pfd_get_async(
      const std::string& app_id, std::string token,
      http2_deferred_response deferred);
  bool dispatch_pfd_app_patch_async(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& patch_body,
      std::string token, http2_deferred_response deferred);
  bool dispatch_pfd_app_delete_async(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, std::string token,
      http2_deferred_response deferred);
  // Nnef-PFD
  bool dispatch_nnef_pfd_put_app_async(
      const std::string& trans_id, const std::string& app_id,
      const nlohmann::json& body, std::string token,
      http2_deferred_response deferred);
  bool dispatch_nnef_pfd_delete_app_async(
      const std::string& trans_id, const std::string& app_id, std::string token,
      http2_deferred_response deferred);

  //
  bool dispatch_pfd_transaction_put_async(
      const std::string& scs_as_id, const std::string& trans_id,
      const nlohmann::json& body, std::string token,
      http2_deferred_response deferred);

  bool dispatch_pfd_transaction_delete_async(
      const std::string& scs_as_id, const std::string& trans_id,
      std::string token, http2_deferred_response deferred);

  bool dispatch_ti_delete_async(
      const std::string& af_id, const std::string& ti_id, std::string token,
      http2_deferred_response deferred);

  bool dispatch_nnef_pfd_put_transaction_async(
      const std::string& trans_id, const nlohmann::json& body,
      std::string token, http2_deferred_response deferred);

  bool dispatch_nnef_pfd_delete_transaction_async(
      const std::string& trans_id, std::string token,
      http2_deferred_response deferred);

  bool dispatch_nnef_pfd_partial_pull_async(
      const nlohmann::json& body, std::string token,
      http2_deferred_response deferred);

 private:
  std::shared_ptr<nef_app> m_app;
  nef_request_dispatcher m_dispatcher;

  // Shared token discipline — called by every execute_* method.
  // Sets token, calls fn(*m_app), and clears token through a local RAII guard
  // even if fn throws.
  //
  // Defined in nef_app_adapter.cpp (after nef_app.hpp is included) so the body
  // sees the complete nef_app type. It is only ever instantiated by the
  // execute_* methods, which all live in the .cpp, so an out-of-line template
  // definition is sufficient (no other TU instantiates it).
  template<typename Fn>
  void execute_with_token(const std::string& token, Fn&& fn);

  // Traffic Influence
  void execute_ti_get(
      const std::string& af_id, const std::string& ti_id,
      const std::string& token, const response_sink& sink);
  void execute_ti_list(
      const std::string& af_id, const std::string& token,
      const response_sink& sink);
  void execute_ti_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
  void execute_ti_update(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);
  void execute_ti_delete(
      const std::string& af_id, const std::string& ti_id,
      const std::string& token, const response_sink& sink);
  void execute_ti_patch(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& patch_body, const std::string& token,
      const response_sink& sink);

  // Monitoring Event
  void execute_monitoring_event_subscribe(
      const std::string& scs_as_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
  void execute_monitoring_event_unsubscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& token, const response_sink& sink);
  void execute_monitoring_event_get(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& token, const response_sink& sink);
  void execute_monitoring_event_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);

  // QoS
  void execute_qos_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
  void execute_qos_delete(
      const std::string& af_id, const std::string& sub_id,
      const std::string& token, const response_sink& sink);
  void execute_qos_get(
      const std::string& af_id, const std::string& sub_id,
      const std::string& token, const response_sink& sink);
  void execute_qos_update(
      const std::string& af_id, const std::string& sub_id,
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);
  void execute_qos_patch(
      const std::string& af_id, const std::string& sub_id,
      const nlohmann::json& patch_body, const std::string& token,
      const response_sink& sink);

  // BDT
  void execute_bdt_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
  void execute_bdt_update(
      const std::string& af_id, const std::string& bdt_id,
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);
  void execute_bdt_delete(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& token, const response_sink& sink);
  void execute_bdt_get(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& token, const response_sink& sink);
  void execute_bdt_patch(
      const std::string& af_id, const std::string& bdt_id,
      const nlohmann::json& patch_body, const std::string& token,
      const response_sink& sink);

  // Analytics
  void execute_analytics_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
  void execute_analytics_delete(
      const std::string& af_id, const std::string& sub_id,
      const std::string& token, const response_sink& sink);
  void execute_analytics_get(
      const std::string& af_id, const std::string& sub_id,
      const std::string& token, const response_sink& sink);
  void execute_analytics_update(
      const std::string& af_id, const std::string& sub_id,
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);
  void execute_analytics_fetch(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);

  // PFD (T8)
  void execute_pfd_create(
      const std::string& app_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
  void execute_pfd_delete(
      const std::string& app_id, const std::string& token,
      const response_sink& sink);
  void execute_pfd_transaction_list(
      const std::string& scs_as_id, const std::string& token,
      const response_sink& sink);
  void execute_pfd_transaction_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);
  void execute_pfd_transaction_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& token, const response_sink& sink);
  void execute_pfd_app_get(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& token,
      const response_sink& sink);
  void execute_pfd_app_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
  void execute_pfd_app_patch(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& patch_body,
      const std::string& token, const response_sink& sink);
  void execute_pfd_app_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& token,
      const response_sink& sink);

  // Nnef_PFDmanagement
  void execute_nnef_pfd_list_transactions(
      const std::string& token, const response_sink& sink);
  void execute_nnef_pfd_put_transaction(
      const std::string& trans_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
  void execute_nnef_pfd_get_transaction(
      const std::string& trans_id, const std::string& token,
      const response_sink& sink);
  void execute_nnef_pfd_delete_transaction(
      const std::string& trans_id, const std::string& token,
      const response_sink& sink);
  void execute_nnef_pfd_get_app(
      const std::string& trans_id, const std::string& app_id,
      const std::string& token, const response_sink& sink);
  void execute_nnef_pfd_put_app(
      const std::string& trans_id, const std::string& app_id,
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);
  void execute_nnef_pfd_delete_app(
      const std::string& trans_id, const std::string& app_id,
      const std::string& token, const response_sink& sink);
  void execute_nnef_pfd_get_applications(
      const std::vector<std::string>& app_ids_filter, const std::string& token,
      const response_sink& sink);
  void execute_nnef_pfd_partial_pull(
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);
  void execute_nnef_pfd_subscription_create(
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);
  void execute_nnef_pfd_subscription_get(
      const std::string& sub_id, const std::string& token,
      const response_sink& sink);
  void execute_nnef_pfd_subscription_put(
      const std::string& sub_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
  void execute_nnef_pfd_subscription_delete(
      const std::string& sub_id, const std::string& token,
      const response_sink& sink);

  // Nnef_EventExposure
  void execute_nnef_event_exposure_subscribe(
      const nlohmann::json& body, const std::string& token,
      const response_sink& sink);
  void execute_nnef_event_exposure_unsubscribe(
      const std::string& subscription_id, const std::string& token,
      const response_sink& sink);
  void execute_nnef_event_exposure_get(
      const std::string& subscription_id, const std::string& token,
      const response_sink& sink);
  void execute_nnef_event_exposure_update(
      const std::string& subscription_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);

  // Inbound NF notification
  void execute_nf_notification(
      const std::string& nf_sub_id, const nlohmann::json& body,
      const std::string& token, const response_sink& sink);
};

}  // namespace oai::nef::app
