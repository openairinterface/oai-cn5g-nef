/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

#ifndef FILE_NEF_HTTP2_SERVER_SEEN
#define FILE_NEF_HTTP2_SERVER_SEEN

#include <nghttp2/asio_http2_server.h>

#include "conversions.hpp"
#include "nef_app.hpp"
#include "uint_generator.hpp"

using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;
using namespace oai::nef::app;

class nef_http2_server {
 public:
  nef_http2_server(std::string addr, uint32_t port, nef_app* nef_app_inst)
      : m_address(addr),
        m_port(port),
        m_server(),
        m_nef_app(nef_app_inst),
        m_running(false) {}

  void start();
  void stop();
  void init(size_t thr) {}

  // ── Monitoring Event ────────────────────────────────────────────────────
  void handle_monitoring_event_subscribe(
      const std::string& scs_as_id, const std::string& body,
      const response& response);

  void handle_monitoring_event_unsubscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      const response& response);

  void handle_monitoring_event_get(
      const std::string& scs_as_id, const std::string& sub_id,
      const response& response);

  // F2.2: Monitoring Event UPDATE (PUT)
  void handle_monitoring_event_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& body, const response& response);

  // ── Nnef_EventExposure (TS 29.591) ─────────────────────────────────────
  void handle_nnef_event_exposure_subscribe(
      const std::string& body, const response& response);

  void handle_nnef_event_exposure_unsubscribe(
      const std::string& subscription_id, const response& response);

  void handle_nnef_event_exposure_get(
      const std::string& subscription_id, const response& response);

  void handle_nnef_event_exposure_update(
      const std::string& subscription_id, const std::string& body,
      const response& response);

  // ── Traffic Influence ───────────────────────────────────────────────────
  void handle_ti_create(
      const std::string& af_id, const std::string& body,
      const response& response);

  // F2.1: TI GET and LIST
  void handle_ti_get(
      const std::string& af_id, const std::string& ti_id,
      const response& response);
  void handle_ti_list(const std::string& af_id, const response& response);

  void handle_ti_update(
      const std::string& af_id, const std::string& ti_id,
      const std::string& body, const response& response);

  void handle_ti_delete(
      const std::string& af_id, const std::string& ti_id,
      const response& response);

  // F2.4: TI PATCH
  void handle_ti_patch(
      const std::string& af_id, const std::string& ti_id,
      const std::string& patch_body, const response& response);

  // ── PFD Management ──────────────────────────────────────────────────────
  void handle_pfd_create(
      const std::string& app_id, const std::string& body,
      const response& response);

  void handle_pfd_delete(const std::string& app_id, const response& response);
  // F2.8: PFD transaction-level and app-level handlers
  void handle_pfd_transaction_list(
      const std::string& scs_as_id, const response& response);

  void handle_pfd_transaction_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& body, const response& response);

  void handle_pfd_transaction_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const response& response);

  void handle_pfd_app_get(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const response& response);

  void handle_pfd_app_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& body,
      const response& response);

  void handle_pfd_app_patch(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& patch_body,
      const response& response);

  void handle_pfd_app_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const response& response);

  // ── Nnef_PFDmanagement ────────────────────────────────────────────────
  void handle_nnef_pfd_list_transactions(const response& response);

  void handle_nnef_pfd_put_transaction(
      const std::string& trans_id, const std::string& body,
      const response& response);

  void handle_nnef_pfd_get_transaction(
      const std::string& trans_id, const response& response);

  void handle_nnef_pfd_delete_transaction(
      const std::string& trans_id, const response& response);

  void handle_nnef_pfd_get_app(
      const std::string& trans_id, const std::string& app_id,
      const response& response);

  void handle_nnef_pfd_put_app(
      const std::string& trans_id, const std::string& app_id,
      const std::string& body, const response& response);

  void handle_nnef_pfd_delete_app(
      const std::string& trans_id, const std::string& app_id,
      const response& response);
  // ── BDT Policy ──────────────────────────────────────────────────────────
  void handle_bdt_create(
      const std::string& af_id, const std::string& body,
      const response& response);

  void handle_bdt_update(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& body, const response& response);

  void handle_bdt_delete(
      const std::string& af_id, const std::string& bdt_id,
      const response& response);

  void handle_bdt_get(
      const std::string& af_id, const std::string& bdt_id,
      const response& response);

  // F2.6: BDT PATCH
  void handle_bdt_patch(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& patch_body, const response& response);

  // ── QoS Monitoring ──────────────────────────────────────────────────────
  void handle_qos_create(
      const std::string& af_id, const std::string& body,
      const response& response);

  void handle_qos_delete(
      const std::string& af_id, const std::string& sub_id,
      const response& response);

  void handle_qos_get(
      const std::string& af_id, const std::string& sub_id,
      const response& response);

  // F2.3: QoS UPDATE (PUT)
  void handle_qos_update(
      const std::string& af_id, const std::string& sub_id,
      const std::string& body, const response& response);

  // F2.5: QoS PATCH
  void handle_qos_patch(
      const std::string& af_id, const std::string& sub_id,
      const std::string& patch_body, const response& response);

  // ── Analytics ───────────────────────────────────────────────────────────
  void handle_analytics_create(
      const std::string& af_id, const std::string& body,
      const response& response);

  void handle_analytics_delete(
      const std::string& af_id, const std::string& sub_id,
      const response& response);

  void handle_analytics_get(
      const std::string& af_id, const std::string& sub_id,
      const response& response);

  // F2.7: Analytics /fetch endpoint
  void handle_analytics_fetch(
      const std::string& af_id, const std::string& body,
      const response& response);

  // F2.9: Analytics UPDATE (PUT)
  void handle_analytics_update(
      const std::string& af_id, const std::string& sub_id,
      const std::string& body, const response& response);

  // ── Inbound NF notification (AMF/SMF/PCF → NEF → AF) ────────────────────
  /**
   * Called when a southbound NF POSTs a notification to:
   *   POST /nef-notify/v1/notify/{nf_sub_id}
   *   POST /nef-notify/v1/notify/amf   (legacy flat-path form)
   *   POST /nef-notify/v1/notify/smf
   */
  void handle_nf_notify(
      const std::string& nf_sub_id, const std::string& body,
      const response& response);

 private:
  std::string m_address;
  uint32_t m_port;
  http2 m_server;
  nef_app* m_nef_app;
  bool m_running;

  oai::utils::uint_generator<uint32_t> m_promise_id_generator;
};

#endif /* FILE_NEF_HTTP2_SERVER_SEEN */
