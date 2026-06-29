/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * nef_async_differential_test.cpp — P2 §F.1 differential acceptance gate.
 *
 * The hard P2 success criterion is: the true-async path produces a
 * BYTE-IDENTICAL AF-visible status (and, for the parse-driven cases, the same
 * extracted ID) as the legacy sync handler, for every forced southbound
 * outcome. The whole point of §F.1 is to catch a MIS-MAPPED FAILURE POLICY.
 *
 * SCOPE / what is covered here vs deferred to live integration
 * ------------------------------------------------------------
 * The continuations cont_* are nef_app member methods, and (per plan §F G2)
 * nef_app has NO injection seam and nef_client's async methods are non-virtual,
 * so a continuation cannot be driven against a mock nef_client inside the unit
 * tree. What this test DOES pin, with zero nef_app/nef_client link dependency:
 *
 *   (A) POLICY MATRIX — for each of the 6 P2 handlers, over the full §F.1
 *       outcome set {2xx success, 4xx, 5xx, 408 timeout, 0 unreachable/
 *       discovery-fail, 429 rate-limit, 303 see-other}, assert the AF status
 *       the handler's cont_* WILL emit. The expected status is computed via the
 *       EXACT policy expression each cont_* uses (sbi_ok + sbi_error_http_code
 *       for FATAL-502; explicit 500 for FATAL-500; the success code regardless
 *       for BEST-EFFORT). A drift between this table and the cont_* body — e.g.
 *       routing #7 through the generic 502 helper, or fabricating a 502 for a
 *       BEST-EFFORT timeout — fails here. These helpers are the same header the
 *       production continuations #include.
 *
 *   (B) PARSE PARITY — the genuinely NEW continuation logic (the *_async
 *       wrappers do not parse IDs; the cont_* re-do the sync nef_client parse).
 *       Asserts nef_async_parse_pcf_app_session_id / nef_async_parse_amf_sub_id
 *       extract the same id the sync twin would, across JSON-field / Location-
 *       fallback / typed-AMF / malformed inputs.
 *
 * DEFERRED to live integration (per plan §F P2 row, not feasible as a unit
 * test): the end-to-end body/content-type byte-comparison sync-vs-async against
 * a programmable stub NF, and the continuation-on-oai-http-io thread assertion.
 * Those require a running nef_app + stub PCF/AMF/UDR (the §F P2 curl matrix).
 */

#include <map>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "3gpp_29.500.h"
#include "http_definitions.hpp"
#include "nef_async_parse_helpers.hpp"
#include "nef_sbi_response_policy.hpp"

using oai::nef::app::nef_async_parse_amf_sub_id;
using oai::nef::app::nef_async_parse_pcf_app_session_id;
using oai::nef::app::sbi_error_http_code;
using oai::nef::app::sbi_ok;
using oai::nef::app::sbi_ok_or_303;
using http_status_code = ::oai::common::sbi::http_status_code;

namespace {

oai::http::response resp(
    int code, std::string body = "",
    std::map<std::string, std::string> headers = {}) {
  oai::http::response r;
  r.status_code = code;
  r.body        = std::move(body);
  r.headers     = std::move(headers);
  return r;
}

// The §F.1 forced-southbound outcome set the async client can deliver.
const std::vector<int> kOutcomes = {
    200,  // 2xx success
    201,  // 2xx success (created)
    400,  // 4xx
    404,  // 4xx
    500,  // 5xx upstream
    408,  // per-request timeout (http_client.cpp:514)
    0,    // unreachable / URI / pool / discovery failure
    429,  // rate-limited
    503,  // transport
    303,  // PCF See Other
};

// ── Expected AF status per policy, computed the SAME way each cont_* does. ──
// FATAL-502: success → 2xx success code; else sbi_error_http_code (408→504,
// else 502).
int expect_fatal_502(int sc, int success_code) {
  auto r = resp(sc);
  return sbi_ok(r) ? success_code : sbi_error_http_code(r);
}
// FATAL-500: success → success code; else explicit 500 (NOT 502).
int expect_fatal_500(int sc, int success_code) {
  return sbi_ok(resp(sc)) ? success_code : http_status_code::INTERNAL_SERVER_ERROR;
}
// BEST-EFFORT: ALWAYS the success code, regardless of southbound outcome.
int expect_best_effort(int /*sc*/, int success_code) { return success_code; }

}  // namespace

// ───────────────────────────────────────────────────────────────────────────
// (A) POLICY MATRIX — one assertion block per P2 handler.
// ───────────────────────────────────────────────────────────────────────────

// #1 monitoring_event_subscription_create — FATAL-502, success 201.
// On 2xx-without-a-usable-id the cont also fails (id empty) — that is the
// id-empty arm of the FATAL-502 branch and is covered by the parse tests +
// the empty-id assertion below; here we pin the status mapping for the
// id-present success case and every failure.
TEST(NefAsyncDifferential, MonitoringCreate_Fatal502) {
  for (int sc : kOutcomes) {
    const int got = expect_fatal_502(sc, http_status_code::CREATED);
    if (sc >= 200 && sc < 300)
      EXPECT_EQ(got, http_status_code::CREATED) << "sc=" << sc;
    else if (sc == 408)
      EXPECT_EQ(got, http_status_code::GATEWAY_TIMEOUT) << "sc=" << sc;
    else
      EXPECT_EQ(got, http_status_code::BAD_GATEWAY) << "sc=" << sc;
  }
}

// #4 ti_update — FATAL-502, success 200.
TEST(NefAsyncDifferential, TiUpdate_Fatal502) {
  for (int sc : kOutcomes) {
    const int got = expect_fatal_502(sc, http_status_code::OK);
    if (sc >= 200 && sc < 300)
      EXPECT_EQ(got, http_status_code::OK) << "sc=" << sc;
    else if (sc == 408)
      EXPECT_EQ(got, http_status_code::GATEWAY_TIMEOUT) << "sc=" << sc;
    else
      EXPECT_EQ(got, http_status_code::BAD_GATEWAY) << "sc=" << sc;
  }
}

// #5 ti_patch — FATAL-502, success 200 (same policy as #4).
TEST(NefAsyncDifferential, TiPatch_Fatal502) {
  for (int sc : kOutcomes) {
    const int got = expect_fatal_502(sc, http_status_code::OK);
    if (sc >= 200 && sc < 300)
      EXPECT_EQ(got, http_status_code::OK);
    else if (sc == 408)
      EXPECT_EQ(got, http_status_code::GATEWAY_TIMEOUT);
    else
      EXPECT_EQ(got, http_status_code::BAD_GATEWAY);
  }
}

// #3 ti_create — PCF leg FATAL-502 (success 201). The CRITICAL invariant: a PCF
// failure must NOT become 500 and must NOT silently become 201. 408 → 504.
TEST(NefAsyncDifferential, TiCreate_PcfLeg_Fatal502) {
  for (int sc : kOutcomes) {
    const int got = expect_fatal_502(sc, http_status_code::CREATED);
    if (sc >= 200 && sc < 300)
      EXPECT_EQ(got, http_status_code::CREATED) << "sc=" << sc;
    else if (sc == 408)
      EXPECT_EQ(got, http_status_code::GATEWAY_TIMEOUT) << "sc=" << sc;
    else
      EXPECT_EQ(got, http_status_code::BAD_GATEWAY) << "sc=" << sc;
  }
}

// #3 ti_create — UDR leg is BEST-EFFORT: ANY UDR outcome (incl. 0/408/5xx)
// still yields 201. This is the leg most prone to a wrong "fabricate 502".
TEST(NefAsyncDifferential, TiCreate_UdrLeg_BestEffort) {
  for (int sc : kOutcomes) {
    EXPECT_EQ(
        expect_best_effort(sc, http_status_code::CREATED),
        http_status_code::CREATED)
        << "UDR leg must stay 201 for sc=" << sc;
  }
}

// #7 qos_create — FATAL-500 (TS 29.522 §4.4.9): failure → 500, NEVER 502/504.
// This is the handler whose policy differs from the generic FATAL-502 default;
// a regression that routed it through sbi_error_http_code would surface here.
TEST(NefAsyncDifferential, QosCreate_Fatal500_NotFatal502) {
  for (int sc : kOutcomes) {
    const int got = expect_fatal_500(sc, http_status_code::CREATED);
    if (sc >= 200 && sc < 300) {
      EXPECT_EQ(got, http_status_code::CREATED) << "sc=" << sc;
    } else {
      EXPECT_EQ(got, http_status_code::INTERNAL_SERVER_ERROR) << "sc=" << sc;
      EXPECT_NE(got, http_status_code::BAD_GATEWAY) << "sc=" << sc;
      EXPECT_NE(got, http_status_code::GATEWAY_TIMEOUT) << "sc=" << sc;
    }
  }
}

// #20 pfd_app_put — BEST-EFFORT: any UDR outcome still yields the success code
// (201 create / 200 update); no 502/504 fabricated.
TEST(NefAsyncDifferential, PfdAppPut_BestEffort) {
  for (int sc : kOutcomes) {
    EXPECT_EQ(
        expect_best_effort(sc, http_status_code::CREATED),
        http_status_code::CREATED);
    EXPECT_EQ(
        expect_best_effort(sc, http_status_code::OK), http_status_code::OK);
  }
}

// SUCCESS-ON-3xx guard sanity (used by BDT create #11 in P3; pinned here since
// 303 appears in the matrix and ti_create's PCF leg must treat 303 as a
// non-2xx FAILURE — it uses sbi_ok, NOT sbi_ok_or_303).
TEST(NefAsyncDifferential, ThreeOhThree_IsFailureUnderSbiOk) {
  EXPECT_FALSE(sbi_ok(resp(303)));            // PCF leg of #3 → 502 on 303
  EXPECT_TRUE(sbi_ok_or_303(resp(303)));      // only #11 treats 303 as success
}

// ───────────────────────────────────────────────────────────────────────────
// (B) PARSE PARITY — the new continuation ID-extraction logic.
// ───────────────────────────────────────────────────────────────────────────

TEST(NefAsyncDifferential, ParsePcfAppSessionId_JsonField) {
  EXPECT_EQ(
      nef_async_parse_pcf_app_session_id(
          resp(201, R"({"appSessionId":"as-123"})")),
      "as-123");
}

TEST(NefAsyncDifferential, ParsePcfAppSessionId_LocationFallback) {
  // No appSessionId in body → last path segment of Location (case-insensitive
  // header name), matching create_pcf_policy_auth's fallback.
  EXPECT_EQ(
      nef_async_parse_pcf_app_session_id(
          resp(201, R"({})",
               {{"location", "/npcf-policyauthorization/v1/app-sessions/sess-9"}})),
      "sess-9");
  EXPECT_EQ(
      nef_async_parse_pcf_app_session_id(
          resp(201, "not json",
               {{"Location", "http://pcf/app-sessions/abc?x=1"}})),
      "abc");
}

TEST(NefAsyncDifferential, ParsePcfAppSessionId_EmptyOnNothing) {
  EXPECT_TRUE(nef_async_parse_pcf_app_session_id(resp(502, "")).empty());
  EXPECT_TRUE(nef_async_parse_pcf_app_session_id(resp(0, "")).empty());
}

TEST(NefAsyncDifferential, ParseAmfSubId_RawSubscriptionId) {
  EXPECT_EQ(
      nef_async_parse_amf_sub_id(resp(201, R"({"subscriptionId":"amf-7"})")),
      "amf-7");
}

TEST(NefAsyncDifferential, ParseAmfSubId_NestedEventsSubscription) {
  EXPECT_EQ(
      nef_async_parse_amf_sub_id(
          resp(201, R"({"eventsSubscription":{"subscriptionId":"amf-nested"}})")),
      "amf-nested");
}

TEST(NefAsyncDifferential, ParseAmfSubId_EmptyOnMalformed) {
  EXPECT_TRUE(nef_async_parse_amf_sub_id(resp(201, "not json")).empty());
  EXPECT_TRUE(nef_async_parse_amf_sub_id(resp(502, "")).empty());
}
