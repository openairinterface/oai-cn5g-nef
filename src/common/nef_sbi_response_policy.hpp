/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
//
// Turning a raw southbound response into a northbound decision.
//
// These are the primitives the cont_* continuations share, not policy in
// themselves: each continuation still picks which one applies (or none, if it
// is best-effort). They live in their own header, rather than in a nef_app.cpp
// anonymous namespace, so the unit tests can reach them without linking the
// whole of nef_client.
//
// They are stateless and touch no nef_app member.

#ifndef NEF_SBI_RESPONSE_POLICY_HPP
#define NEF_SBI_RESPONSE_POLICY_HPP

#include "3gpp_29.500.h"         // http_status_code
#include "http_definitions.hpp"  // oai::sba::response

namespace oai::nef::app {

// Self-contained alias so this header does not depend on a TU-level
// `using namespace oai::common::sbi;` being in scope at the include site.
using http_status_code = ::oai::common::sbi::http_status_code;

// Plain 2xx success, the same guard the synchronous handlers use.
inline bool sbi_ok(const oai::sba::response& r) {
  return r.status_code >= 200 && r.status_code < 300;
}

// BDT create only: PCF answers a successful create with 303 See Other.
inline bool sbi_ok_or_303(const oai::sba::response& r) {
  return sbi_ok(r) || r.status_code == http_status_code::SEE_OTHER;
}

// The default failure mapping: a per-request timeout becomes 504, and
// everything else non-2xx collapses to 502 — including status 0, which is how
// the async path reports a URI-parse, pool or discovery failure.
//
// Only for continuations that use this default. The ones that propagate the
// upstream status, answer 500, or treat 3xx as success reproduce their own
// synchronous branch instead.
inline int sbi_error_http_code(const oai::sba::response& r) {
  if (r.status_code == http_status_code::REQUEST_TIMEOUT)
    return http_status_code::GATEWAY_TIMEOUT;  // 408 -> 504
  return http_status_code::BAD_GATEWAY;        // everything else non-2xx -> 502
}

}  // namespace oai::nef::app

#endif  // NEF_SBI_RESPONSE_POLICY_HPP
