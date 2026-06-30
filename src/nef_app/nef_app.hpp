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
#include "nef_request_task.hpp"  // response_sink (true-async entry/cont_* sink)
#include "nef_subscription.hpp"
#include "uint_generator.hpp"

namespace bs2 = boost::signals2;

namespace oai {
namespace http {
struct response;  // fwd-decl for true-async cont_* signatures (full type in
                  // .cpp)
}  // namespace http
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
  [[nodiscard]] std::string get_request_bearer_token() const;

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

  // ───────────────────────────────────────────────────────────────────────────
  // True-async handler split (plan 20260625-nef-true-async-all-apis, §A).
  //
  // Each in-scope sync handler above is mirrored by an entry/cont_* pair:
  //   * <op>()    runs on the DISPATCHER worker. It does ALL token-dependent
  //   work
  //               (authorize/validate/local-store), captures the state the
  //               continuation needs BY VALUE, FIRES the async southbound call,
  //               and RETURNS — never parked on the SBI round-trip.
  //   * cont_*    runs on an oai-http-io thread (or inline on the synchronous
  //               error fast-path). It holds NO bearer token, re-locks every
  //               store it touches, applies the handler's §D southbound-failure
  //               POLICY, builds the SAME (status, body) the sync handler did,
  //               and completes the deferred via the response_sink.
  //
  // The legacy sync handle_* methods above stay intact; adapter execute paths
  // and direct-call unit tests still use them while true async coverage grows.
  // P2 = the 6 proving-ground handlers (#1,#3,#4,#5,#7,#20).

  // #1 monitoring create — single, FATAL-502.
  void monitoring_event_subscribe(
      const std::string& scs_as_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_monitoring_event_subscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, oai::http::response r, response_sink sink);

  // #7 qos create — single, FATAL-500 (TS 29.522 §4.4.9). The Location/self
  // header is produced by the adapter's qos-create header sink.
  void qos_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_qos_create(
      const std::string& af_id, const std::string& qos_sub_id,
      nlohmann::json req_data_json, oai::http::response r, response_sink sink);

  // #3 traffic-influence create — CHAINED PCF→UDR. phase-1 pre-resolves PCF+UDR
  // endpoints and fires PCF create via *_at_async; cont_ti_create_pcf applies
  // the FATAL-502 PCF policy + §A.2b re-check, then fires the BEST-EFFORT UDR
  // put-influence via *_at_async; cont_ti_create_udr sends the 201.
  void ti_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_ti_create_pcf(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& ti_id, const std::string& pcf_ep,
      const std::string& udr_ep, std::shared_ptr<nef_subscription> ti_sub,
      oai::http::response r, response_sink sink);
  void cont_ti_create_udr(
      const nlohmann::json& body, const std::string& ti_id,
      oai::http::response r, response_sink sink);

  // #4 traffic-influence update — single, FATAL-502.
  void ti_update(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void cont_ti_update(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& body, oai::http::response r, response_sink sink);

  // #5 traffic-influence patch — single, FATAL-502.
  void ti_patch(
      const std::string& af_id, const std::string& ti_id,
      const nlohmann::json& patch_body, const std::string& token,
      response_sink sink);
  void cont_ti_patch(
      const std::string& af_id, const std::string& ti_id,
      const std::string& app_session_id, nlohmann::json patched_copy,
      oai::http::response r, response_sink sink);

  // #20 PFD app put — single, BEST-EFFORT.
  void pfd_app_put(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_pfd_app_put(
      const std::string& scs_as_id, const std::string& app_id,
      nlohmann::json new_app_json, bool is_create, oai::http::response r,
      response_sink sink);

  // ═══════════════════════════════════════════════════════════════════════════
  // P3 — single-call Units 1-4 (15 handlers). Same entry/cont_* split as P2.
  // Sync handle_* methods stay intact (legacy + tests). Policy per plan §D.
  // ═══════════════════════════════════════════════════════════════════════════

  // ── Unit 1: Monitoring + QoS deletes/updates ──────────────────────────────
  // #2 monitoring delete — single, BEST-EFFORT (204). Empty sink.
  void monitoring_event_unsubscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& token, response_sink sink);
  void cont_monitoring_event_unsubscribe(
      const std::string& scs_as_id, const std::string& sub_id,
      const std::string& nf_sub_id, oai::http::response r, response_sink sink);

  // #8 qos update (PUT) — single, BEST-EFFORT (200).
  void qos_update(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void cont_qos_update(
      const std::string& scs_as_id, const std::string& sub_id,
      nlohmann::json response_data, oai::http::response r, response_sink sink);

  // #9 qos patch — single, BEST-EFFORT (200).
  void qos_patch(
      const std::string& scs_as_id, const std::string& sub_id,
      const nlohmann::json& patch_body, const std::string& token,
      response_sink sink);
  void cont_qos_patch(
      const std::string& scs_as_id, const std::string& sub_id,
      nlohmann::json patched, oai::http::response r, response_sink sink);

  // #10 qos delete — single, BEST-EFFORT (204). Empty sink.
  void qos_delete(
      const std::string& af_id, const std::string& qos_sub_id,
      const std::string& token, response_sink sink);
  void cont_qos_delete(
      const std::string& af_id, const std::string& qos_sub_id,
      const std::string& nf_sub_id, oai::http::response r, response_sink sink);

  // ── Unit 2: BDT (all PCF) ─────────────────────────────────────────────────
  // #11 bdt create — single, SUCCESS-ON-3xx (201/303→201, else 502). Header
  // sink (x-deprecated). bdt_id generated in phase-1.
  void bdt_create(
      const std::string& af_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_bdt_create(
      const std::string& af_id, const std::string& bdt_id,
      oai::_3gpp::model::BdtPolicy bdt_policy, oai::http::response r,
      response_sink sink);

  // #12 bdt update — single, FATAL-502. Header sink (x-deprecated).
  void bdt_update(
      const std::string& af_id, const std::string& bdt_id,
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void cont_bdt_update(
      const std::string& af_id, const std::string& bdt_id,
      oai::_3gpp::model::BdtPolicy bdt_policy, oai::http::response r,
      response_sink sink);

  // #13 bdt patch — single, FATAL-502. JSON sink.
  void bdt_patch(
      const std::string& af_id, const std::string& bdt_policy_id,
      const nlohmann::json& patch_body, const std::string& token,
      response_sink sink);
  void cont_bdt_patch(
      const std::string& af_id, const std::string& bdt_policy_id,
      nlohmann::json patched_copy, oai::http::response r, response_sink sink);

  // #14 bdt delete — single, BEST-EFFORT (204). Empty sink (x-deprecated).
  void bdt_delete(
      const std::string& af_id, const std::string& bdt_id,
      const std::string& token, response_sink sink);
  void cont_bdt_delete(
      const std::string& af_id, const std::string& bdt_id,
      oai::http::response r, response_sink sink);

  // ── Unit 3: PFD T8 single (UDR) ───────────────────────────────────────────
  // #15 pfd create — single, BEST-EFFORT (201). JSON sink.
  void pfd_create(
      const std::string& app_id, const nlohmann::json& body,
      const std::string& token, response_sink sink);
  void cont_pfd_create(
      const std::string& app_id, nlohmann::json body, oai::http::response r,
      response_sink sink);

  // #16 pfd delete — single, BEST-EFFORT (204). Empty sink.
  void pfd_delete(
      const std::string& app_id, const std::string& token, response_sink sink);
  void cont_pfd_delete(
      const std::string& app_id, oai::http::response r, response_sink sink);

  // #17 pfd get — single read, FATAL-propagate (404→404 / 2xx→200 / else→502).
  // JSON sink. NOTE: handle_pfd_get is currently unrouted (no server shim / no
  // sync adapter entry); the split + dispatch are added for completeness.
  void pfd_get(
      const std::string& app_id, const std::string& token, response_sink sink);
  void cont_pfd_get(
      const std::string& app_id, oai::http::response r, response_sink sink);

  // #21 pfd app patch — single, BEST-EFFORT (200). JSON sink.
  void pfd_app_patch(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const nlohmann::json& patch_body,
      const std::string& token, response_sink sink);
  void cont_pfd_app_patch(
      const std::string& scs_as_id, const std::string& app_id,
      nlohmann::json patched, oai::http::response r, response_sink sink);

  // #22 pfd app delete — single, BEST-EFFORT (204). Empty sink.
  void pfd_app_delete(
      const std::string& scs_as_id, const std::string& trans_id,
      const std::string& app_id, const std::string& token, response_sink sink);
  void cont_pfd_app_delete(
      const std::string& scs_as_id, const std::string& app_id,
      oai::http::response r, response_sink sink);

  // ── Unit 4: Nnef-PFD single (UDR) ─────────────────────────────────────────
  // #25 nnef pfd put app — single, BEST-EFFORT (201/200). JSON sink.
  void nnef_pfd_put_app(
      const std::string& transaction_id, const std::string& app_id,
      const nlohmann::json& body, const std::string& token, response_sink sink);
  void cont_nnef_pfd_put_app(
      const std::string& app_id, nlohmann::json response_app,
      nlohmann::json normalized_app, bool is_create, oai::http::response r,
      response_sink sink);

  // #26 nnef pfd delete app — single, BEST-EFFORT (204). Empty sink.
  void nnef_pfd_delete_app(
      const std::string& transaction_id, const std::string& app_id,
      const std::string& token, response_sink sink);
  void cont_nnef_pfd_delete_app(
      const std::string& app_id, oai::http::response r, response_sink sink);

  // ═══════════════════════════════════════════════════════════════════════════
  // P4 — chained Unit 5 (6 handlers). Each handler drives a shared_ptr cursor
  // through its southbound chain; every continuation fires the next *_at_async
  // leg on a phase-1-pre-resolved endpoint (NO discover_nf on oai-http-io,
  // §A.1c). The response_sink lives inside the cursor and is moved into the
  // final step → auto-500-on-drop covers any abandoned path. Sync handle_*
  // methods stay intact (legacy + tests). Implemented in ascending complexity,
  // #23 LAST.
  // ═══════════════════════════════════════════════════════════════════════════

  // ── #19 pfd_transaction_delete — N-loop UDR DELETE, no compensation,
  //    BEST-EFFORT 204 (mirrors handle_pfd_transaction_delete :3490-3520).
  //    The transaction is removed from local state in phase-1 under m_pfd_mutex
  //    (the snapshot of app_ids is the delete work set); the cursor fires one
  //    udr_delete_pfd_data_at_async per app (failures ignored, idempotent) and
  //    the final step sends 204.
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

  // ── #24 nnef_pfd_delete_transaction — same N-loop UDR DELETE cursor, no
  //    compensation, BEST-EFFORT 204 (mirrors
  //    handle_nnef_pfd_delete_transaction :3880-3911). Reuses PfdDeleteChain;
  //    the final step sends 204 + audit log.
  void nnef_pfd_delete_transaction(
      const std::string& transaction_id, const std::string& token,
      response_sink sink);
  void nnef_pfd_delete_transaction_step(std::shared_ptr<PfdDeleteChain> st);

  // ── #27 nnef_pfd_partial_pull — N-loop UDR GET (reads), BEST-EFFORT per-app
  //    (mirrors handle_nnef_pfd_partial_pull :4083-4129). §A.5 SNAPSHOT rule:
  //    phase-1 builds the iteration snapshot under
  //    m_nnef_pfd_transactions_mutex (applying requested_ids), copies {app_id,
  //    fallback_app_data}, RELEASES the lock, then runs the read cursor. Each
  //    step fires udr_get_pfd_data_at_async; the continuation pushes (sbi_ok ?
  //    parsed : fallback) with
  //    ["applicationId"]=app_id; the final step sends 200 with the array.
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

  // ── #18 pfd_transaction_put — N-loop UDR PUT + compensating DELETE rollback,
  //    FATAL-500 (mirrors handle_pfd_transaction_put :3382-3487). §A.5
  //    PfdPutChain cursor: pfd_put_step fires udr_put_pfd_data_at_async; on
  //    step-k failure → pfd_put_rollback issues udr_delete_pfd_data_at_async
  //    over committed[0..k-1] in REVERSE, then sends 500 from the last rollback
  //    continuation; on full success it commits local state + 201/200.
  //    Two-phase ONLY (no phase-C).
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

  // ── #6 traffic_influence_delete — chained PCF→UDR (both deletes),
  // BEST-EFFORT
  //    204 (mirrors handle_traffic_influence_delete :1992-2050). phase-1
  //    resolves PCF+UDR endpoints; cont fires delete_pcf_policy_auth_at_async
  //    (when a PCF policy exists) then udr_delete_influence_data_at_async;
  //    failures logged, response reflects overall success (always 204);
  //    idempotent, no compensation; empty sink. Local state erased in phase-1.
  void ti_delete(
      const std::string& af_id, const std::string& ti_id,
      const std::string& token, response_sink sink);
  void cont_ti_delete_pcf(
      const std::string& af_id, const std::string& ti_id,
      const std::string& udr_ep, oai::http::response r, response_sink sink);
  void cont_ti_delete_udr(
      const std::string& af_id, const std::string& ti_id, oai::http::response r,
      response_sink sink);

  // ── #23 nnef_pfd_put_transaction — THREE-PHASE (≠ #18, see §A.5b). Mirrors
  //    handle_nnef_pfd_put_transaction :3753-3854. Phase A = N-loop UDR PUT
  //    (NnefPutChain); Phase B = compensating DELETE rollback on PUT failure →
  //    500 (FATAL-500); Phase C = POST-COMMIT best-effort removed-app cleanup:
  //    after PUT loop succeeds AND local state committed, SEND the success
  //    response (201/200), then fire fire-and-forget
  //    udr_delete_pfd_data_at_async for each app in removed_apps (computed in
  //    phase-1 under the mutex). Phase-C continuations capture ONLY value
  //    copies (tid, app_id) — NOT st, NOT the sink — WARN-only on failure,
  //    NEVER re-touch the spent deferred.
  struct NnefPutChain {
    std::string transaction_id;
    bool is_create = false;
    std::string udr_ep;
    nlohmann::json transaction;   // built in phase-1; the success body
    nlohmann::json applications;  // the PUT set (object: app_id -> app_body)
    std::vector<std::string> app_ids;  // ordered PUT cursor over applications
    std::size_t idx = 0;
    std::vector<std::string> committed;     // phase B rollback set
    std::vector<std::string> removed_apps;  // phase C cleanup set (prior \ new)
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
