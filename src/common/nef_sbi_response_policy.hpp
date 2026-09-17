/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
//
// Turning a raw southbound response into a northbound decision.
//
// These are primitives the cont_* continuations share, not policy in
// themselves. Each continuation still picks which one applies — or none at
// all, when it is best-effort.
//
// They live in their own header, rather than in a nef_app.cpp anonymous
// namespace, so the unit tests can reach them without linking the whole of
// nef_client. They are stateless and touch no nef_app member.

#ifndef NEF_SBI_RESPONSE_POLICY_HPP
#define NEF_SBI_RESPONSE_POLICY_HPP

#include "3gpp_29.500.h"         // http_status_code
#include "http_definitions.hpp"  // oai::sba::response

namespace oai::nef::app {

// Self-contained alias so this header does not depend on a TU-level
// `using namespace oai::common::sbi;` being in scope at the include site.
using http_status_code = ::oai::common::sbi::http_status_code;

// Plain 2xx success — the same guard the synchronous handlers use.
inline bool sbi_ok(const oai::sba::response& r) {
  return r.status_code >= 200 && r.status_code < 300;
}

// BDT create only: PCF answers a successful create with 303 See Other.
inline bool sbi_ok_or_303(const oai::sba::response& r) {
  return sbi_ok(r) || r.status_code == http_status_code::SEE_OTHER;
}

// The default failure mapping:
//
//   408 REQUEST_TIMEOUT   -> 504 GATEWAY_TIMEOUT  (a per-request timeout)
//   any other non-2xx     -> 502 BAD_GATEWAY
//
// Status 0 lands in the 502 bucket; that is how the async path reports a
// URI-parse, pool or discovery failure.
//
// Watch the 408 case: it is easy to miss that this can return 504. The
// callers currently pair the returned code with a hardcoded "Bad Gateway"
// ProblemDetails title, so a southbound timeout goes out as a 504 titled
// "Bad Gateway". Pass a title that matches the code you got back.
//
// Only for continuations that take this default. The ones that propagate the
// upstream status, answer 500, or treat 3xx as success reproduce their own
// synchronous branch instead.
inline int sbi_error_http_code(const oai::sba::response& r) {
  if (r.status_code == http_status_code::REQUEST_TIMEOUT)
    return http_status_code::GATEWAY_TIMEOUT;  // 408 -> 504
  return http_status_code::BAD_GATEWAY;        // everything else non-2xx -> 502
}

}  // namespace oai::nef::app

#endif  // NEF_SBI_RESPONSE_POLICY_HPP
