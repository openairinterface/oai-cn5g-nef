/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
//
// NEF true-async southbound-response policy helpers
// (plan 20260625-nef-true-async-all-apis, §C.1).
//
// These are the PURE, reusable mapping helpers each `cont_*` continuation uses
// to translate one raw southbound `oai::http::response` into a
// deferred-response decision. They carry NO state and touch NO nef_app member —
// they are split into this header (rather than living in a nef_app.cpp
// anonymous namespace) so they can be unit-tested directly without the
// monolithic nef_client link closure (see test/nef_sbi_response_policy_test.cpp
// and plan §F G2).
//
// SCOPE NOTE (P1): this header is the SHARED infra the handler phases (P2-P4)
// build on. It defines ONLY the mapping helpers, NOT any per-handler policy.
// Each continuation still chooses WHICH helper (or none — BEST-EFFORT) to call
// per the §D failure-policy column; this header just supplies the primitives:
//
//   * sbi_ok            — 2xx success predicate (every FATAL/propagate cont).
//   * sbi_ok_or_303     — SUCCESS-ON-3xx predicate, BDT create (#11) ONLY:
//                         a PCF 303 See Other is success, not a failure.
//   * sbi_error_http_code — the FATAL-502 DEFAULT status mapping: 408 -> 504,
//                         everything else non-2xx (0 / 4xx / 429 / 503 / 5xx)
//                         -> 502. Used ONLY by FATAL-502 continuations; the
//                         FATAL-500 / FATAL-propagate / SUCCESS-ON-3xx /
//                         BEST-EFFORT policies do NOT route through it.

#ifndef NEF_SBI_RESPONSE_POLICY_HPP
#define NEF_SBI_RESPONSE_POLICY_HPP

#include "3gpp_29.500.h"         // http_status_code
#include "http_definitions.hpp"  // oai::http::response

namespace oai::nef::app {

// Self-contained alias so this header does not depend on a TU-level
// `using namespace oai::common::sbi;` being in scope at the include site.
using http_status_code = ::oai::common::sbi::http_status_code;

// 2xx success predicate. Mirrors the sync handlers' success guard (e.g.
// monitoring create nef_app.cpp:1576, TI create :1795).
inline bool sbi_ok(const oai::http::response& r) {
  return r.status_code >= 200 && r.status_code < 300;
}

// SUCCESS-ON-3xx variant used ONLY by BDT create (#11): treat a PCF
// 303 See Other as success (mirrors nef_app.cpp:2205-2207, the
// `status_code != SEE_OTHER` guard).
inline bool sbi_ok_or_303(const oai::http::response& r) {
  return sbi_ok(r) || r.status_code == http_status_code::SEE_OTHER;
}

// FATAL-502 DEFAULT mapping (§C.1). Maps a verified per-request timeout
// (408, http_client.cpp:514) to 504 Gateway Timeout; every other non-2xx
// input the async path can deliver — 0 (URI-parse / pool / discovery
// failure), 400, 429 rate-limit, 503 transport, and any upstream 4xx/5xx —
// collapses to 502 Bad Gateway. FATAL-500 / FATAL-propagate / SUCCESS-ON-3xx
// / BEST-EFFORT continuations MUST NOT call this; they reproduce their own
// sync branch (see plan §D policy column).
inline int sbi_error_http_code(const oai::http::response& r) {
  if (r.status_code == http_status_code::REQUEST_TIMEOUT)
    return http_status_code::GATEWAY_TIMEOUT;  // 408 -> 504
  return http_status_code::BAD_GATEWAY;        // everything else non-2xx -> 502
}

}  // namespace oai::nef::app

#endif  // NEF_SBI_RESPONSE_POLICY_HPP
