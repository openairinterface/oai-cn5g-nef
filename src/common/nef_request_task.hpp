/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
#pragma once
#include <functional>
#include <map>
#include <string>

namespace oai::nef::app {

// Response sink: called once nef_app has produced a result. It is captured
// into the task by value, so it must own everything it touches.
//
// Warning: this one type is used under two different contracts. The type
// does not tell you which one applies; the call path does. Getting it wrong
// shows up as a hung worker or an uncaught exception, not a compile error.
//
// (A) Deferred sinks, used on every dispatch_*_async path.
//     Built by the make_deferred_*_sink helpers in nef_app_adapter.cpp.
//     Each wraps a shared_ptr<http2_deferred_response>, whose contract is
//     documented in common-src/nghttp/http2_server.h.
//       - Callable once, from any thread. The call is a CAS on a shared
//         atomic, so a second call is a silent no-op, not an error.
//       - Does not block: it writes into the work item, posts delivery onto
//         the libevent loop with event_base_once(), and returns.
//       - If the sink is destroyed without ever being called, the deferred
//         handle's destructor posts a 500, so the client is not left
//         waiting. Forgetting to call it is a bug, but not a fatal one.
//
// (B) Blocking sinks, used on every dispatch_* path that is not async.
//     Built inline in api-server/nef-http2-server.cpp by dispatch_and_wait,
//     dispatch_and_wait_empty and dispatch_and_wait_headers, plus a
//     hand-written copy inside handle_nf_notify. Each captures an
//     http2_response& and a std::promise<void>&, and the HTTP worker then
//     waits on fut.wait().
//       - Must be called exactly once. Both halves of that rule are strict,
//         and the type enforces neither.
//       - A second call makes promise::set_value() throw
//         promise_already_satisfied on a dispatcher worker, and nothing on
//         that path catches it.
//       - If it is never called, the HTTP worker blocks forever, because
//         fut.wait() has no timeout. The pool has hardware_concurrency()
//         threads, so a few of these exhaust the server.
//       - Both captures live on the HTTP worker's stack, so the sink must
//         not outlive the dispatch call. Do not store it, queue it, or pass
//         it to a continuation.
//
// Rule of thumb while both contracts exist: write every method that takes a
// sink to the stricter contract of (B), calling it exactly once on every
// path, early returns included. It is then correct under (A) as well. The
// long-term fix is to move (B) onto (A) so there is only one contract. Until
// then, only the call path tells you which contract an endpoint uses.
using response_sink = std::function<void(int status_code, std::string body)>;

}  // namespace oai::nef::app
