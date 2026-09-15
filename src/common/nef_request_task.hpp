/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
#pragma once
#include <functional>
#include <map>
#include <string>

namespace oai::nef::app {

// Response sink: invoked once nef_app has produced a result. It is captured
// into the task by value, so it must own everything it touches.
//
// WARNING: this one typedef carries TWO incompatible contracts. The type
// does not tell you which one you are holding -- the call path does. Get it
// wrong and the failure is a hung worker or an uncaught exception, not a
// compile error.
//
// (A) DEFERRED sinks -- every dispatch_*_async path.
//     Built by the make_deferred_*_sink helpers in nef_app_adapter.cpp.
//     Each wraps a shared_ptr<http2_deferred_response>, whose contract is
//     documented in common-src/sba/http2_server.h.
//       - Callable exactly once from ANY thread. The call is a CAS on a
//         shared atomic; a second call is a silent no-op, not an error.
//       - Non-blocking: it writes into the work item, posts delivery onto
//         the libevent loop via event_base_once(), and returns.
//       - If the sink is destroyed having never been called, the deferred
//         handle's destructor posts a 500, so the client is not left
//         hanging. Forgetting to call it is a bug, but a survivable one.
//
// (B) BLOCKING sinks -- every non-async dispatch_* path.
//     Built inline in api-server/nef-http2-server.cpp by dispatch_and_wait,
//     dispatch_and_wait_empty, dispatch_and_wait_headers, and one
//     hand-rolled fourth copy inside handle_nf_notify. Each captures an
//     http2_response& and a std::promise<void>&, and the HTTP worker then
//     parks on fut.wait().
//       - Callable exactly once, and it MUST be called. Both conditions
//         are hard, and neither is enforced by the type.
//       - Call it twice and promise::set_value() throws
//         promise_already_satisfied on a dispatcher worker. There is no
//         catch anywhere on that path.
//       - Never call it and the HTTP worker blocks forever: fut.wait() has
//         no timeout. The pool is hardware_concurrency() threads, so a
//         handful of these exhausts the server.
//       - Both captures are stack objects in the HTTP worker's frame, so
//         the sink must not outlive the dispatch call. Do not store it, do
//         not queue it, do not hand it to a continuation.
//
// Rule of thumb while the split exists: write every sink-taking method to
// (B)'s stricter contract -- exactly once, on every path including every
// early return -- and it will be correct under (A) as well. The long-term
// fix is to collapse (B) onto (A) so there is only one contract; until
// that lands, an endpoint's mode is a property of its call path only.
using response_sink = std::function<void(int status_code, std::string body)>;

}  // namespace oai::nef::app
