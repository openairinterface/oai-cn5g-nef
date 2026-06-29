/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
//
// P1 unit test for the NEF true-async southbound-response policy helpers
// (plan 20260625-nef-true-async-all-apis, §C.1).
//
// WHAT THIS TEST PINS
// -------------------
// The pure mapping helpers in nef_sbi_response_policy.hpp are the SHARED infra
// every `cont_*` continuation (P2-P4) leans on to translate one raw southbound
// `oai::http::response` into a deferred-response decision. They are pure (no
// nef_app state, no link closure) so they are tested directly here — the
// highest-value, lowest-cost P1 verification.
//
//   * sbi_error_http_code  — FATAL-502 default: 408 -> 504; every other non-2xx
//                            (0 / 4xx / 429 / 503 / 5xx) -> 502. This is the
//                            most regression-prone mapping (the 408->504 special
//                            case is easy to drop), so it is exhaustively pinned.
//   * sbi_ok               — strict 2xx success predicate.
//   * sbi_ok_or_303        — SUCCESS-ON-3xx (BDT create #11): 2xx OR exactly 303.

#include <gtest/gtest.h>

#include "3gpp_29.500.h"
#include "http_definitions.hpp"
#include "nef_sbi_response_policy.hpp"

using oai::common::sbi::http_status_code;
using oai::http::response;
using oai::nef::app::sbi_error_http_code;
using oai::nef::app::sbi_ok;
using oai::nef::app::sbi_ok_or_303;

namespace {

response with_status(int code) {
  response r;
  r.status_code = code;
  return r;
}

// ---- sbi_ok : strict 2xx --------------------------------------------------
TEST(SbiOk, TwoHundredRangeIsOk) {
  EXPECT_TRUE(sbi_ok(with_status(200)));
  EXPECT_TRUE(sbi_ok(with_status(201)));
  EXPECT_TRUE(sbi_ok(with_status(204)));
  EXPECT_TRUE(sbi_ok(with_status(299)));
}

TEST(SbiOk, NonTwoHundredIsNotOk) {
  EXPECT_FALSE(sbi_ok(with_status(0)));    // sync fast-path / discovery fail
  EXPECT_FALSE(sbi_ok(with_status(199)));
  EXPECT_FALSE(sbi_ok(with_status(300)));
  EXPECT_FALSE(sbi_ok(with_status(303)));  // 303 is NOT ok for sbi_ok
  EXPECT_FALSE(sbi_ok(with_status(404)));
  EXPECT_FALSE(sbi_ok(with_status(500)));
  EXPECT_FALSE(sbi_ok(with_status(502)));
}

// ---- sbi_ok_or_303 : SUCCESS-ON-3xx (BDT create #11) ----------------------
TEST(SbiOkOr303, TwoHundredRangeIsOk) {
  EXPECT_TRUE(sbi_ok_or_303(with_status(200)));
  EXPECT_TRUE(sbi_ok_or_303(with_status(201)));
  EXPECT_TRUE(sbi_ok_or_303(with_status(299)));
}

TEST(SbiOkOr303, ExactlyThreeOhThreeIsOk) {
  EXPECT_TRUE(sbi_ok_or_303(with_status(http_status_code::SEE_OTHER)));  // 303
  EXPECT_TRUE(sbi_ok_or_303(with_status(303)));
}

TEST(SbiOkOr303, OtherThreeXxIsNotOk) {
  EXPECT_FALSE(sbi_ok_or_303(with_status(300)));
  EXPECT_FALSE(sbi_ok_or_303(with_status(301)));
  EXPECT_FALSE(sbi_ok_or_303(with_status(302)));
  EXPECT_FALSE(sbi_ok_or_303(with_status(307)));
  EXPECT_FALSE(sbi_ok_or_303(with_status(308)));
}

TEST(SbiOkOr303, NonSuccessIsNotOk) {
  EXPECT_FALSE(sbi_ok_or_303(with_status(0)));
  EXPECT_FALSE(sbi_ok_or_303(with_status(404)));
  EXPECT_FALSE(sbi_ok_or_303(with_status(502)));
}

// ---- sbi_error_http_code : FATAL-502 default mapping ----------------------
TEST(SbiErrorHttpCode, TimeoutMapsTo504) {
  // 408 Request Timeout (verified async path value, http_client.cpp:514)
  // -> 504 Gateway Timeout.
  EXPECT_EQ(
      sbi_error_http_code(with_status(http_status_code::REQUEST_TIMEOUT)),
      http_status_code::GATEWAY_TIMEOUT);
  EXPECT_EQ(sbi_error_http_code(with_status(408)), 504);
}

TEST(SbiErrorHttpCode, SyncFastPathZeroMapsTo502) {
  // status 0 = URI-parse / pool / discovery failure delivered synchronously.
  EXPECT_EQ(sbi_error_http_code(with_status(0)), http_status_code::BAD_GATEWAY);
}

TEST(SbiErrorHttpCode, TransportAndRateLimitMapTo502) {
  EXPECT_EQ(sbi_error_http_code(with_status(429)), 502);  // rate limited
  EXPECT_EQ(sbi_error_http_code(with_status(503)), 502);  // transport
}

TEST(SbiErrorHttpCode, UpstreamFourAndFiveXxMapTo502) {
  EXPECT_EQ(sbi_error_http_code(with_status(400)), 502);
  EXPECT_EQ(sbi_error_http_code(with_status(404)), 502);
  EXPECT_EQ(sbi_error_http_code(with_status(409)), 502);
  EXPECT_EQ(sbi_error_http_code(with_status(500)), 502);
  EXPECT_EQ(sbi_error_http_code(with_status(501)), 502);
}

TEST(SbiErrorHttpCode, OnlyFourZeroEightBecomes504) {
  // Exhaustively confirm no other status near the timeout boundary maps to 504.
  for (int code = 400; code <= 599; ++code) {
    const int mapped = sbi_error_http_code(with_status(code));
    if (code == 408)
      EXPECT_EQ(mapped, http_status_code::GATEWAY_TIMEOUT) << "code=" << code;
    else
      EXPECT_EQ(mapped, http_status_code::BAD_GATEWAY) << "code=" << code;
  }
}

}  // namespace
