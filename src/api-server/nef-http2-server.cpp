/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef-http2-server.h"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include <future>
#include "3gpp_29.500.h"
#include "Helpers.h"
#include "logger.hpp"
#include "nef_config.hpp"
#include "nef_health_check.hpp"
#include "nef_rate_limiter.hpp"
#include "nef_sbi_helper.hpp"

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

using namespace oai::nef::app;
using namespace oai::common::sbi;
using oai::nef::api::nef_sbi_helper;

namespace {

//------------------------------------------------------------------------------
static void end_http2_error(
    http2_response& res, int status, const std::string& title,
    const std::string& detail) {
  nlohmann::json pd;
  pd["type"]   = "about:blank";
  pd["title"]  = title;
  pd["status"] = status;
  pd["detail"] = detail;
  res.send(status, {{"content-type", "application/problem+json"}}, pd.dump());
}

//------------------------------------------------------------------------------
static bool end_http2_if_draining(
    const std::atomic<bool>& draining, http2_response& res) {
  if (draining.load(std::memory_order_relaxed)) {
    end_http2_error(
        res, http_status_code::SERVICE_UNAVAILABLE, "Service Unavailable",
        "Server is draining");
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
static bool end_http2_if_rate_limited(
    const http2_request& req, const std::string& bearer_token,
    http2_response& res) {
  const std::string& rate_key =
      !bearer_token.empty() ? bearer_token : req.peer_address;
  if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
    end_http2_error(
        res, http_status_code::TOO_MANY_REQUESTS, "Too Many Requests",
        "Rate limit exceeded");
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
// Parse a request body into `out`. On false the 400 has already been sent, so
// the caller can just return.
static bool parse_body_or_400_detail(
    const std::string& body, nlohmann::json& out, http2_response& res) {
  try {
    out = nlohmann::json::parse(body);
  } catch (const nlohmann::json::exception& e) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return false;
  }
  return true;
}

//------------------------------------------------------------------------------
// Pull the token out of a "Bearer <token>" Authorization header. The keyword
// is matched case-insensitively.
static std::string extract_bearer(const http2_request& req) {
  auto it = req.headers.find("authorization");
  if (it == req.headers.end()) return "";
  const std::string& auth          = it->second;
  static const std::string kBearer = "Bearer ";
  if (auth.size() <= kBearer.size()) return "";
  for (size_t i = 0; i < kBearer.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(auth[i])) !=
        std::tolower(static_cast<unsigned char>(kBearer[i])))
      return "";
  }
  return auth.substr(kBearer.size());
}

//------------------------------------------------------------------------------
// Synchronous dispatch: hand dispatch_fn a sink that writes into this
// worker's http2_response, then park on the future until the sink fires.
//
// Returns false if the dispatcher refused the work; the 503 has already been
// sent, so the caller can just return.
//
// The sink captures this frame's response and promise, so it must not outlive
// the call — see the blocking-sink contract on response_sink.
template<typename DispatchFn>
static bool dispatch_and_wait(DispatchFn&& dispatch_fn, http2_response& res) {
  std::promise<void> done;
  auto fut  = done.get_future();
  auto sink = [&res, &done](int code, std::string body) mutable {
    res.send(code, {{"content-type", "application/json"}}, std::move(body));
    done.set_value();
  };
  const auto st = std::forward<DispatchFn>(dispatch_fn)(std::move(sink));
  if (st != oai::nef::app::nef_app_adapter::dispatch_status::ok) {
    end_http2_error(
        res, http_status_code::SERVICE_UNAVAILABLE, "Service Unavailable",
        "Server is overloaded, please retry later");
    return false;
  }
  fut.wait();
  return true;
}

//------------------------------------------------------------------------------
// Same, for delete-style handlers: the sink delivers an empty body, forwarded
// unchanged with no content-type and whatever headers the caller passed in.
template<typename DispatchFn>
static bool dispatch_and_wait_empty(
    DispatchFn&& dispatch_fn, http2_response& res,
    std::map<std::string, std::string> headers = {}) {
  std::promise<void> done;
  auto fut  = done.get_future();
  auto sink = [&res, &done, headers](int code, std::string /*body*/) mutable {
    res.send(code, headers, "");
    done.set_value();
  };
  const auto st = std::forward<DispatchFn>(dispatch_fn)(std::move(sink));
  if (st != oai::nef::app::nef_app_adapter::dispatch_status::ok) {
    end_http2_error(
        res, http_status_code::SERVICE_UNAVAILABLE, "Service Unavailable",
        "Server is overloaded, please retry later");
    return false;
  }
  fut.wait();
  return true;
}

//------------------------------------------------------------------------------
// Same, for handlers whose response headers depend on what the adapter
// produced — a Location on 201, say, or an x-deprecated marker.
//
// header_fn sees the status and the parsed body. The body is mutable on
// purpose: this is where a relative `self` gets rewritten to an absolute URI.
// It is re-serialized after header_fn returns.
template<typename DispatchFn, typename HeaderFn>
static bool dispatch_and_wait_headers(
    DispatchFn&& dispatch_fn, http2_response& res, HeaderFn&& header_fn) {
  std::promise<void> done;
  auto fut  = done.get_future();
  auto sink = [&res, &done, &header_fn](int code, std::string body) mutable {
    nlohmann::json resp_body;
    if (!body.empty()) {
      try {
        resp_body = nlohmann::json::parse(body);
      } catch (...) {
        resp_body = nlohmann::json::object();
      }
    }
    std::map<std::string, std::string> headers = header_fn(code, resp_body);
    res.send(code, headers, resp_body.dump());
    done.set_value();
  };
  const auto st = std::forward<DispatchFn>(dispatch_fn)(std::move(sink));
  if (st != oai::nef::app::nef_app_adapter::dispatch_status::ok) {
    end_http2_error(
        res, http_status_code::SERVICE_UNAVAILABLE, "Service Unavailable",
        "Server is overloaded, please retry later");
    return false;
  }
  fut.wait();
  return true;
}

}  // namespace

namespace oai::nef::api {

//------------------------------------------------------------------------------
// TI GET
void nef_http2_server::handle_ti_get(
    const std::string& af_id, const std::string& ti_id,
    const std::string& bearer_token, http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_ti_get(
            af_id, ti_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
// TI LIST
void nef_http2_server::handle_ti_list(
    const std::string& af_id, const std::string& bearer_token,
    http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_ti_list(
            af_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
// Monitoring Event UPDATE (PUT)
void nef_http2_server::handle_monitoring_event_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_monitoring_event_update(
            scs_as_id, sub_id, json_body, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_update(
    const std::string& af_id, const std::string& sub_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_qos_update_async(
      af_id, sub_id, json_body, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
// BDT PATCH
void nef_http2_server::handle_bdt_patch(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& patch_body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_patch;
  if (!parse_body_or_400_detail(patch_body, json_patch, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_bdt_patch_async(
      af_id, bdt_id, json_patch, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
// Analytics /fetch
void nef_http2_server::handle_analytics_fetch(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_analytics_fetch(
            af_id, json_body, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_subscribe(
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  auto header_fn = [](int http_code, nlohmann::json& resp_body) {
    std::map<std::string, std::string> headers;
    headers["content-type"] = "application/json";
    if (http_code == http_status_code::CREATED && resp_body.contains("self") &&
        resp_body["self"].is_string()) {
      headers["location"] = resp_body["self"].get<std::string>();
    }
    return headers;
  };
  dispatch_and_wait_headers(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_event_exposure_subscribe(
            json_body, bearer_token, std::move(sink));
      },
      res, header_fn);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id, const std::string& bearer_token,
    http2_response& res) {
  dispatch_and_wait_empty(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_event_exposure_unsubscribe(
            subscription_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_get(
    const std::string& subscription_id, const std::string& bearer_token,
    http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_event_exposure_get(
            subscription_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_update(
    const std::string& subscription_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_event_exposure_update(
            subscription_id, json_body, bearer_token, std::move(sink));
      },
      res);
}

// ---------------------------------------------------------------------------
// Request admission preamble
// ---------------------------------------------------------------------------

// Runs the admission steps every service route shares, in this exact order:
//   1. take the bearer token off the request,
//   2. answer 503 and stop while the server is draining,
//   3. answer 429 and stop when this caller is rate limited.
//
// Returns true when the route may continue. Returns false once a guard has
// answered the request, and the caller must then return immediately.
//
// Eleven of the twelve routes open with this call; the preamble was
// byte-identical in all of them. route_health() is the twelfth and
// deliberately does not use it -- see the note there.
bool nef_http2_server::begin_request(
    const http2_request& req, http2_response& res, std::string& bearer_token) {
  bearer_token = extract_bearer(req);
  if (end_http2_if_draining(m_draining, res)) return false;
  if (end_http2_if_rate_limited(req, bearer_token, res)) return false;
  return true;
}

// Resolves the nine configured API base paths. Runs once, at the top of
// start() and before server_.start() admits any request.
//
// They are read-only for the whole serving lifetime, which is why the route_*
// members read them without synchronisation.
void nef_http2_server::build_api_base_paths() {
  const std::string api_version =
      nef_config_inst->nef()->get_sbi().get_api_version();
  m_nnef_event_exposure_base =
      nef_sbi_helper::NnefEventExposureBase + api_version;
  m_nef_monitoring_event_base =
      nef_sbi_helper::NefMonitoringEventBase + api_version;
  m_nef_traffic_influence_base =
      nef_sbi_helper::NefTrafficInfluenceBase + api_version;
  m_nef_pfd_management_base =
      nef_sbi_helper::NefPfdManagementBase + api_version;
  m_nnef_pfd_management_base =
      nef_sbi_helper::NnefPfdManagementBase + api_version;
  m_nef_bdt_base = nef_sbi_helper::NefBdtBase + api_version;
  m_nef_qos_monitoring_base =
      nef_sbi_helper::NefQosMonitoringBase + api_version;
  m_nef_analytics_base = nef_sbi_helper::NefAnalyticsBase + api_version;
  m_nef_notify_base    = nef_sbi_helper::NefNotifyBase + api_version;
}

// ---------------------------------------------------------------------------
// The routing table
// ---------------------------------------------------------------------------

void nef_http2_server::start() {
  Logger::nef_sbi().info(
      "NEF HTTP/2 server listening on %s:%u", m_address.c_str(), m_port);

  build_api_base_paths();

  // ------------------------------------------------------------------------
  // One row per registered path prefix, in registration order. Each row's
  // handler is a route_* member defined immediately below this function.
  //
  // ORDER IS PART OF THE CONTRACT. oai::sba::http2_server matches by prefix.
  // http2_server::start() sorts the registered table longest-prefix-first
  // with a NON-STABLE std::sort, so registration order only decides ties
  // between prefixes of equal length. Append new rows at the end; do not
  // reorder these to group services together.
  //
  // Each route owns its own path parsing, and the twelve do NOT parse alike:
  // three different prefix-strip strategies, and different segment indices
  // read out of the split. Nothing beyond begin_request() is shared between
  // them -- see the comment on each member.
  // ------------------------------------------------------------------------
  using route_handler =
      void (nef_http2_server::*)(const http2_request&, http2_response&);

  const struct {
    std::string prefix;
    route_handler handler;
  } routes[] = {
      // Nnef_EventExposure (TS 29.591)
      //   /nnef-eventexposure/v1/subscriptions[/{subscriptionId}]
      {m_nnef_event_exposure_base + nef_sbi_helper::NefPathSubscriptions,
       &nef_http2_server::route_nnef_event_exposure},
      // Monitoring Event (TS 29.122)
      //   /3gpp-monitoring-event/v1/{scsAsId}/subscriptions[/{subId}]
      {m_nef_monitoring_event_base + "/",
       &nef_http2_server::route_monitoring_event},
      // Traffic Influence (TS 29.522)
      //   /3gpp-traffic-influence/v1/{afId}/subscriptions[/{appSessionId}]
      {m_nef_traffic_influence_base + "/",
       &nef_http2_server::route_traffic_influence},
      // PFD Management (TS 29.122)
      //   /3gpp-pfd-management/v1/{scsAsId}/transactions
      //       [/{transId}[/applications/{appId}]]
      {m_nef_pfd_management_base + "/",
       &nef_http2_server::route_pfd_management},
      // Nnef_PFDmanagement (TS 29.551)
      //   /nnef-pfdmanagement/v1/transactions
      //       [/{transId}[/applications/{appId}]]
      {m_nnef_pfd_management_base +
           nef_sbi_helper::NnefPfdManagementPathTransactions,
       &nef_http2_server::route_nnef_pfd_transactions},
      // Nnef_PFDmanagement (TS 29.551)
      //   /nnef-pfdmanagement/v1/applications[/partial-pull]
      {m_nnef_pfd_management_base +
           nef_sbi_helper::NnefPfdManagementPathApplications,
       &nef_http2_server::route_nnef_pfd_applications},
      // Nnef_PFDmanagement (TS 29.551)
      //   /nnef-pfdmanagement/v1/subscriptions[/{subId}]
      {m_nnef_pfd_management_base +
           nef_sbi_helper::NnefPfdManagementPathSubscriptions,
       &nef_http2_server::route_nnef_pfd_subscriptions},
      // BDT Policy (TS 29.122 5.13)
      //   /3gpp-bdt/v1/{scsAsId}/[policies|bdtPolicies][/{polId}]
      {m_nef_bdt_base + "/", &nef_http2_server::route_bdt},
      // AsSessionWithQoS (TS 29.122)
      //   /3gpp-as-session-with-qos/v1/{afId}/subscriptions[/{subId}]
      {m_nef_qos_monitoring_base + "/",
       &nef_http2_server::route_qos_monitoring},
      // Analytics Exposure (TS 29.522)
      //   /3gpp-analyticsexposure/v1/{afId}/[fetch|subscriptions[/{subId}]]
      {m_nef_analytics_base + "/", &nef_http2_server::route_analytics},
      // Inbound southbound-NF notification sink
      //   /nef-notify/v1/notify/{nf_sub_id}  (AMF/SMF/PCF -> NEF)
      {m_nef_notify_base + nef_sbi_helper::NefNotifyPathNotify,
       &nef_http2_server::route_nf_notify},
      // Health check / readiness probe
      //   /health  (no bearer, no drain guard, no rate limit -- see below)
      {nef_sbi_helper::NefHealthPath, &nef_http2_server::route_health},
  };

  for (const auto& route : routes) {
    // Copy the member pointer by value: the lambda outlives this loop.
    const route_handler handler = route.handler;
    server_.handle(
        route.prefix,
        [this, handler](const http2_request& req, http2_response& res) {
          (this->*handler)(req, res);
        });
  }

  construct_dispatch_adapter();

  // Start the server (blocks until stop() is called)
  server_.start();
}

// Builds the dispatch adapter, before serving starts.
//
// The dispatcher pool is sized slightly larger than the HTTP worker pool, so
// that HTTP workers parked on fut.wait() are not the bottleneck. The size is
// configurable via dispatcher_pool_size; the DEFAULT (override == 0) stays
// http_workers + 2, byte-identical to prior behavior.
//
// NOTE: until an in-flight cap exists, this pool is the de-facto concurrency
// limiter. Do not shrink it below the auto default without that cap.
void nef_http2_server::construct_dispatch_adapter() {
  const auto& srv_cfg            = server_.config();
  const std::size_t http_workers = srv_cfg.num_worker_threads;
  const std::size_t disp_threads =
      srv_cfg.dispatcher_pool_size > 0 ?
          static_cast<std::size_t>(srv_cfg.dispatcher_pool_size) :
          http_workers + 2;
  m_adapter =
      std::make_unique<nef_app_adapter>(m_nef_app, disp_threads, http_workers);
}

// ---------------------------------------------------------------------------
// Route handlers, in registration order
// ---------------------------------------------------------------------------

// Nnef_EventExposure (TS 29.591)
//   /nnef-eventexposure/v1/subscriptions[/{subscriptionId}]
void nef_http2_server::route_nnef_event_exposure(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  const std::string pfx = m_nnef_event_exposure_base + "/";
  const auto pfx_pos    = req.path.find(pfx);
  if (pfx_pos == std::string::npos) {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
    return;
  }

  auto rest = req.path.substr(pfx_pos + pfx.size());
  std::vector<std::string> path_parts;
  boost::split(path_parts, rest, boost::is_any_of("/"));
  auto resource        = !path_parts.empty() ? path_parts[0] : "";
  auto subscription_id = (path_parts.size() > 1) ? path_parts[1] : "";

  if (resource != nef_sbi_helper::NefResourceSubscriptions) {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
    return;
  }

  if (req.method == method_e::POST && subscription_id.empty()) {
    handle_nnef_event_exposure_subscribe(req.body, bearer_token, res);
  } else if (req.method == method_e::GET && !subscription_id.empty()) {
    handle_nnef_event_exposure_get(subscription_id, bearer_token, res);
  } else if (req.method == method_e::PUT && !subscription_id.empty()) {
    handle_nnef_event_exposure_update(
        subscription_id, req.body, bearer_token, res);
  } else if (req.method == method_e::DELETE && !subscription_id.empty()) {
    handle_nnef_event_exposure_unsubscribe(subscription_id, bearer_token, res);
  } else {
    end_http2_error(
        res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
        "HTTP method is not supported for this resource");
  }
}

// Monitoring Event (TS 29.122)
//   /3gpp-monitoring-event/v1/{scsAsId}/subscriptions[/{subId}]
void nef_http2_server::route_monitoring_event(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  // /{base}/{ver}/{scsAsId}/subscriptions[/{subId}]
  const std::string pfx = m_nef_monitoring_event_base + "/";
  auto rest             = req.path.substr(req.path.find(pfx) + pfx.size());
  // rest = scsAsId[/subscriptions[/subId]]
  std::vector<std::string> path_parts;
  boost::split(path_parts, rest, boost::is_any_of("/"));
  auto scs_as_id = !path_parts.empty() ? path_parts[0] : "";
  auto sub_id    = (path_parts.size() > 2) ? path_parts[2] : "";

  if (req.method == method_e::POST) {
    handle_monitoring_event_subscribe(scs_as_id, req.body, bearer_token, res);
  } else if (req.method == method_e::DELETE && !sub_id.empty()) {
    handle_monitoring_event_unsubscribe(scs_as_id, sub_id, bearer_token, res);
  } else if (req.method == method_e::PUT && !sub_id.empty()) {
    handle_monitoring_event_update(
        scs_as_id, sub_id, req.body, bearer_token, res);
  } else if (req.method == method_e::GET) {
    handle_monitoring_event_get(scs_as_id, sub_id, bearer_token, res);
  } else {
    end_http2_error(
        res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
        "HTTP method is not supported for this resource");
  }
}

// Traffic Influence (TS 29.522)
//   /3gpp-traffic-influence/v1/{afId}/subscriptions[/{appSessionId}]
void nef_http2_server::route_traffic_influence(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  const std::string pfx = m_nef_traffic_influence_base + "/";
  auto rest             = req.path.substr(req.path.find(pfx) + pfx.size());
  std::vector<std::string> path_parts;
  boost::split(path_parts, rest, boost::is_any_of("/"));
  auto af_id                 = !path_parts.empty() ? path_parts[0] : "";
  std::string sub_path       = (path_parts.size() > 1) ? path_parts[1] : "";
  std::string app_session_id = (path_parts.size() > 2) ? path_parts[2] : "";

  if (sub_path == nef_sbi_helper::NefResourceSubscriptions) {
    if (req.method == method_e::GET && app_session_id.empty()) {
      handle_ti_list(af_id, bearer_token, res);
    } else if (req.method == method_e::GET && !app_session_id.empty()) {
      handle_ti_get(af_id, app_session_id, bearer_token, res);
    } else if (req.method == method_e::POST && app_session_id.empty()) {
      handle_ti_create(af_id, req.body, bearer_token, res);
    } else if (req.method == method_e::PUT && !app_session_id.empty()) {
      handle_ti_update(af_id, app_session_id, req.body, bearer_token, res);
    } else if (req.method == method_e::PATCH && !app_session_id.empty()) {
      handle_ti_patch(af_id, app_session_id, req.body, bearer_token, res);
    } else if (req.method == method_e::DELETE && !app_session_id.empty()) {
      handle_ti_delete(af_id, app_session_id, bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported for this resource");
    }
  } else {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
  }
}

// PFD Management (TS 29.122)
//   /3gpp-pfd-management/v1/{scsAsId}/transactions
//       [/{transId}[/applications/{appId}]]
void nef_http2_server::route_pfd_management(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  const std::string pfx = m_nef_pfd_management_base + "/";
  auto rest             = req.path.substr(req.path.find(pfx) + pfx.size());
  // rest = {scsAsId}/transactions[/{transId}[/applications/{appId}]]
  std::vector<std::string> path_parts;
  boost::split(path_parts, rest, boost::is_any_of("/"));
  auto scs_as_id = !path_parts.empty() ? path_parts[0] : "";
  auto top_seg   = (path_parts.size() > 1) ? path_parts[1] : "";
  auto trans_id  = (path_parts.size() > 2) ? path_parts[2] : "";
  std::string app_id;
  if (path_parts.size() > 4 &&
      path_parts[3] == nef_sbi_helper::NefResourceApplications) {
    app_id = path_parts[4];
  }

  if (top_seg != nef_sbi_helper::NefResourceTransactions) {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
    return;
  }

  if (trans_id.empty()) {
    if (req.method == method_e::GET) {
      handle_pfd_transaction_list(scs_as_id, bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported for this resource");
    }
  } else if (app_id.empty()) {
    if (req.method == method_e::PUT) {
      handle_pfd_transaction_put(
          scs_as_id, trans_id, req.body, bearer_token, res);
    } else if (req.method == method_e::DELETE) {
      handle_pfd_transaction_delete(scs_as_id, trans_id, bearer_token, res);
    } else if (req.method == method_e::GET) {
      // Legacy app-level GET: answered from the transaction list.
      handle_pfd_transaction_list(scs_as_id, bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported for this resource");
    }
  } else {
    if (req.method == method_e::GET) {
      handle_pfd_app_get(scs_as_id, trans_id, app_id, bearer_token, res);
    } else if (req.method == method_e::PUT) {
      handle_pfd_app_put(
          scs_as_id, trans_id, app_id, req.body, bearer_token, res);
    } else if (req.method == method_e::PATCH) {
      handle_pfd_app_patch(
          scs_as_id, trans_id, app_id, req.body, bearer_token, res);
    } else if (req.method == method_e::DELETE) {
      handle_pfd_app_delete(scs_as_id, trans_id, app_id, bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported for this resource");
    }
  }
}

// Nnef_PFDmanagement (TS 29.551)
//   /nnef-pfdmanagement/v1/transactions[/{transId}[/applications/{appId}]]
void nef_http2_server::route_nnef_pfd_transactions(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  const std::string pfx = m_nnef_pfd_management_base +
                          nef_sbi_helper::NnefPfdManagementPathTransactions;
  const auto pfx_pos = req.path.find(pfx);
  if (pfx_pos == std::string::npos) {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
    return;
  }
  auto rest = req.path.substr(pfx_pos + pfx.size());
  std::vector<std::string> path_parts;
  if (!rest.empty() && rest[0] == '/') {
    boost::split(path_parts, rest.substr(1), boost::is_any_of("/"));
  }
  const bool has_transaction_tail =
      !path_parts.empty() && !path_parts[0].empty();
  if (!has_transaction_tail) {
    if (req.method == method_e::GET) {
      handle_nnef_pfd_list_transactions(bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported for this resource");
    }
    return;
  }
  const auto trans_id = path_parts[0];
  if (trans_id.empty()) {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
    return;
  }
  if (path_parts.size() <= 1) {
    if (req.method == method_e::GET) {
      handle_nnef_pfd_get_transaction(trans_id, bearer_token, res);
    } else if (req.method == method_e::PUT) {
      handle_nnef_pfd_put_transaction(trans_id, req.body, bearer_token, res);
    } else if (req.method == method_e::DELETE) {
      handle_nnef_pfd_delete_transaction(trans_id, bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported for this resource");
    }
    return;
  }
  if (path_parts.size() <= 2 ||
      path_parts[1] != nef_sbi_helper::NefResourceApplications) {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
    return;
  }
  const auto app_id = path_parts[2];
  if (app_id.empty()) {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
    return;
  }
  if (req.method == method_e::GET) {
    handle_nnef_pfd_get_app(trans_id, app_id, bearer_token, res);
  } else if (req.method == method_e::PUT) {
    handle_nnef_pfd_put_app(trans_id, app_id, req.body, bearer_token, res);
  } else if (req.method == method_e::DELETE) {
    handle_nnef_pfd_delete_app(trans_id, app_id, bearer_token, res);
  } else {
    end_http2_error(
        res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
        "HTTP method is not supported for this resource");
  }
}

// Nnef_PFDmanagement (TS 29.551)
//   /nnef-pfdmanagement/v1/applications[/partial-pull]
void nef_http2_server::route_nnef_pfd_applications(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  const std::string partial_pull_path =
      nef_sbi_helper::NnefPfdManagementPathPartialPull;
  if (req.path.size() >= partial_pull_path.size() &&
      req.path.substr(req.path.size() - partial_pull_path.size()) ==
          partial_pull_path) {
    if (req.method == method_e::POST) {
      handle_nnef_pfd_partial_pull(req.body, bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported");
    }
    return;
  }
  if (req.method == method_e::GET) {
    std::vector<std::string> ids;
    std::string q = req.raw_query;
    while (!q.empty()) {
      auto amp   = q.find('&');
      auto token = (amp != std::string::npos) ? q.substr(0, amp) : q;
      q          = (amp != std::string::npos) ? q.substr(amp + 1) : "";
      const std::string key = "app-ids=";
      if (token.substr(0, key.size()) == key)
        ids.push_back(token.substr(key.size()));
    }
    handle_nnef_pfd_get_applications(ids, bearer_token, res);
  } else {
    end_http2_error(
        res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
        "HTTP method is not supported");
  }
}

// Nnef_PFDmanagement (TS 29.551)
//   /nnef-pfdmanagement/v1/subscriptions[/{subId}]
void nef_http2_server::route_nnef_pfd_subscriptions(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  const std::string pfx = m_nnef_pfd_management_base +
                          nef_sbi_helper::NnefPfdManagementPathSubscriptions;
  auto rest = req.path.substr(pfx.size());
  std::vector<std::string> path_parts;
  if (!rest.empty() && rest[0] == '/') {
    boost::split(path_parts, rest.substr(1), boost::is_any_of("/"));
  }
  if (rest.empty() || rest == "/") {
    if (req.method == method_e::POST) {
      handle_nnef_pfd_subscription_create(req.body, bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported");
    }
    return;
  }
  const auto sub_id = !path_parts.empty() ? path_parts[0] : rest;
  if (sub_id.empty()) {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
    return;
  }
  if (req.method == method_e::GET) {
    handle_nnef_pfd_subscription_get(sub_id, bearer_token, res);
  } else if (req.method == method_e::PUT) {
    handle_nnef_pfd_subscription_put(sub_id, req.body, bearer_token, res);
  } else if (req.method == method_e::DELETE) {
    handle_nnef_pfd_subscription_delete(sub_id, bearer_token, res);
  } else {
    end_http2_error(
        res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
        "HTTP method is not supported");
  }
}

// BDT Policy (TS 29.122 5.13)
//   /3gpp-bdt/v1/{scsAsId}/[policies|bdtPolicies][/{polId}]
void nef_http2_server::route_bdt(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  const std::string pfx = m_nef_bdt_base + "/";
  auto rest             = req.path.substr(req.path.find(pfx) + pfx.size());
  std::vector<std::string> path_parts;
  boost::split(path_parts, rest, boost::is_any_of("/"));
  auto af_id       = !path_parts.empty() ? path_parts[0] : "";
  auto policy_path = (path_parts.size() > 1) ? path_parts[1] : "";
  auto pol_id      = (path_parts.size() > 2) ? path_parts[2] : "";
  bool legacy_path = (policy_path == nef_sbi_helper::NefResourcePolicies);
  if (legacy_path) {
    Logger::nef_sbi().warn(
        "HTTP/2: BDT request on deprecated path '/policies' "
        "(af_id='%s'); "
        "please migrate to canonical '/bdtPolicies' path (TS 29.122 "
        "§5.13)",
        af_id.c_str());
  }
  if (req.method == method_e::PATCH &&
      policy_path == nef_sbi_helper::NefResourceBdtPolicies &&
      !pol_id.empty()) {
    handle_bdt_patch(af_id, pol_id, req.body, bearer_token, res);
  } else if (req.method == method_e::POST) {
    handle_bdt_create(af_id, req.body, bearer_token, res, legacy_path);
  } else if (req.method == method_e::GET && pol_id.empty()) {
    handle_bdt_get(af_id, "", bearer_token, res, legacy_path);
  } else if (req.method == method_e::GET && !pol_id.empty()) {
    handle_bdt_get(af_id, pol_id, bearer_token, res, legacy_path);
  } else if (req.method == method_e::PUT && !pol_id.empty()) {
    handle_bdt_update(af_id, pol_id, req.body, bearer_token, res, legacy_path);
  } else if (req.method == method_e::DELETE && !pol_id.empty()) {
    handle_bdt_delete(af_id, pol_id, bearer_token, res, legacy_path);
  } else {
    end_http2_error(
        res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
        "HTTP method is not supported for this resource");
  }
}

// AsSessionWithQoS (TS 29.122)
//   /3gpp-as-session-with-qos/v1/{afId}/subscriptions[/{subId}]
void nef_http2_server::route_qos_monitoring(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  const std::string pfx = m_nef_qos_monitoring_base + "/";
  auto rest             = req.path.substr(req.path.find(pfx) + pfx.size());
  std::vector<std::string> path_parts;
  boost::split(path_parts, rest, boost::is_any_of("/"));
  auto af_id    = !path_parts.empty() ? path_parts[0] : "";
  auto sub_path = (path_parts.size() > 1) ? path_parts[1] : "";
  auto sub_id   = (path_parts.size() > 2) ? path_parts[2] : "";
  if (sub_path == nef_sbi_helper::NefResourceSubscriptions) {
    if (req.method == method_e::POST && sub_id.empty()) {
      handle_qos_create(af_id, req.body, bearer_token, res);
    } else if (req.method == method_e::GET && sub_id.empty()) {
      handle_qos_get(af_id, "", bearer_token, res);
    } else if (req.method == method_e::GET && !sub_id.empty()) {
      handle_qos_get(af_id, sub_id, bearer_token, res);
    } else if (req.method == method_e::PUT && !sub_id.empty()) {
      handle_qos_update(af_id, sub_id, req.body, bearer_token, res);
    } else if (req.method == method_e::PATCH && !sub_id.empty()) {
      handle_qos_patch(af_id, sub_id, req.body, bearer_token, res);
    } else if (req.method == method_e::DELETE && !sub_id.empty()) {
      handle_qos_delete(af_id, sub_id, bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported for this resource");
    }
  } else {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
  }
}

// Analytics Exposure (TS 29.522)
//   /3gpp-analyticsexposure/v1/{afId}/[fetch|subscriptions[/{subId}]]
void nef_http2_server::route_analytics(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  const std::string pfx = m_nef_analytics_base + "/";
  auto rest             = req.path.substr(req.path.find(pfx) + pfx.size());
  std::vector<std::string> path_parts;
  boost::split(path_parts, rest, boost::is_any_of("/"));
  auto af_id    = !path_parts.empty() ? path_parts[0] : "";
  auto sub_path = (path_parts.size() > 1) ? path_parts[1] : "";
  auto sub_id   = (path_parts.size() > 2) ? path_parts[2] : "";
  if (path_parts.size() == 2 && sub_path == nef_sbi_helper::NefResourceFetch &&
      req.method == method_e::POST) {
    handle_analytics_fetch(af_id, req.body, bearer_token, res);
  } else if (sub_path == nef_sbi_helper::NefResourceSubscriptions) {
    if (req.method == method_e::POST && sub_id.empty()) {
      handle_analytics_create(af_id, req.body, bearer_token, res);
    } else if (req.method == method_e::GET && sub_id.empty()) {
      handle_analytics_get(af_id, "", bearer_token, res);
    } else if (req.method == method_e::GET && !sub_id.empty()) {
      handle_analytics_get(af_id, sub_id, bearer_token, res);
    } else if (req.method == method_e::PUT && !sub_id.empty()) {
      handle_analytics_update(af_id, sub_id, req.body, bearer_token, res);
    } else if (req.method == method_e::DELETE && !sub_id.empty()) {
      handle_analytics_delete(af_id, sub_id, bearer_token, res);
    } else {
      end_http2_error(
          res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
          "HTTP method is not supported for this resource");
    }
  } else {
    end_http2_error(
        res, http_status_code::NOT_FOUND, "Not Found",
        "Requested resource was not found");
  }
}

// Inbound southbound-NF notification sink
//   POST /nef-notify/v1/notify/{nf_sub_id}   (AMF/SMF/PCF -> NEF -> AF)
void nef_http2_server::route_nf_notify(
    const http2_request& req, http2_response& res) {
  std::string bearer_token;
  if (!begin_request(req, res, bearer_token)) return;

  if (req.method == method_e::POST) {
    const std::string prefix =
        m_nef_notify_base + nef_sbi_helper::NefNotifyPathNotify + "/";
    std::string nf_sub_id;
    auto pos = req.path.find(prefix);
    if (pos != std::string::npos) {
      nf_sub_id = req.path.substr(pos + prefix.size());
      while (!nf_sub_id.empty() && nf_sub_id.back() == '/')
        nf_sub_id.pop_back();
    }
    Logger::nef_sbi().debug(
        "Received NF notification for sub-id: %s", nf_sub_id.c_str());
    handle_nf_notify(nf_sub_id, req.body, bearer_token, res);
  } else {
    end_http2_error(
        res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
        "HTTP method is not supported for this resource");
  }
}

// Health check / readiness probe
//   GET /health
//
// Deliberately does NOT run begin_request(). Two reasons:
//   - it takes no bearer token;
//   - it reads m_draining and reports it, so drain-guarding it would destroy
//     the endpoint: it must keep answering precisely while draining.
// Do not unify this route with the other eleven.
void nef_http2_server::route_health(
    const http2_request& req, http2_response& res) {
  if (req.method != method_e::GET) {
    end_http2_error(
        res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
        "HTTP method is not supported for this resource");
    return;
  }
  const bool draining = m_draining.load(std::memory_order_relaxed);
  const int uptime =
      static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - m_start_time)
                           .count());
  const std::string instance_id =
      m_nef_app ? m_nef_app->get_nef_instance_id() : "";
  int http_code          = http_status_code::NO_RESPONSE;
  const std::string body = oai::nef::app::nef_health_check::make_response(
      draining, instance_id, uptime, http_code);
  res.send(http_code, {{"content-type", "application/json"}}, body);
}

void nef_http2_server::stop() {
  // With async dispatch the default mode, teardown order matters: HTTP intake
  // must stop BEFORE the dispatcher drains, so that no new work_items or
  // southbound fires arrive while the dispatcher is draining.
  //
  //   1. Stop HTTP intake. server_.stop() schedules GOAWAY and the drain timer
  //      on the event loop. No new work_items reach the HTTP worker pool, so
  //      new southbound fires stop arriving.
  //   2. Drain and join the dispatcher pool. m_adapter->stop() runs all queued
  //      phase-1 tasks to completion, then joins. Intake is already stopped,
  //      so the only fires left are continuation-chained ones from requests
  //      already in flight.
  //   3. Stop and join the oai-http-io pool. NOT done here: it happens on the
  //      http_client_impl dtor path (http_client_inst release in main.cpp).
  //      io_service::stop() ABORTS outstanding handlers there, so orphaned
  //      deferred handles complete via auto-500-on-drop.
  //   4. Join the HTTP worker pool last. That happens inside server_.start()
  //      once the event loop exits (http2_server.cpp pool_.reset()), joined
  //      via nef_http2_manager.join() in main.cpp.
  server_.stop();                    // step 1: stop intake first
  if (m_adapter) m_adapter->stop();  // step 2: drain + join dispatcher
}

//------------------------------------------------------------------------------
void nef_http2_server::initiate_graceful_shutdown() {
  m_draining.store(true, std::memory_order_relaxed);
  Logger::nef_sbi().info(
      "NEF HTTP/2 server: graceful shutdown initiated — "
      "new requests will receive 503 Service Unavailable");
}

//------------------------------------------------------------------------------
// Handler implementations
void nef_http2_server::handle_monitoring_event_subscribe(
    const std::string& scs_as_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_monitoring_event_subscribe_async(
      scs_as_id, json_body, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_monitoring_event_unsubscribe(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_monitoring_event_unsubscribe_async(
      scs_as_id, sub_id, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_monitoring_event_get(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_monitoring_event_get(
            scs_as_id, sub_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_create(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_ti_create_async(
      af_id, json_body, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_update(
    const std::string& af_id, const std::string& ti_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_ti_update_async(
      af_id, ti_id, json_body, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_delete(
    const std::string& af_id, const std::string& ti_id,
    const std::string& bearer_token, http2_response& res) {
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_ti_delete_async(
      af_id, ti_id, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nf_notify(
    const std::string& nf_sub_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!body.empty()) {
    try {
      json_body = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
      Logger::nef_sbi().warn(
          "Failed to parse NF notification body: %s", e.what());
      end_http2_error(
          res, http_status_code::BAD_REQUEST, "Bad Request",
          "Missing or invalid request payload");
      return;
    }
  }
  // The adapter maps nef_app's bool result to 204 (found) / 404 (not found).
  std::promise<void> done;
  auto fut  = done.get_future();
  auto sink = [&res, &done, nf_sub_id](int code, std::string /*body*/) mutable {
    if (code == http_status_code::NOT_FOUND) {
      end_http2_error(
          res, http_status_code::NOT_FOUND, "Not Found",
          "No subscription found for notification id: " + nf_sub_id);
    } else {
      res.send(code, {}, "");
    }
    done.set_value();
  };
  const auto st = m_adapter->dispatch_nf_notification(
      nf_sub_id, json_body, bearer_token, std::move(sink));
  if (st != nef_app_adapter::dispatch_status::ok) {
    end_http2_error(
        res, http_status_code::SERVICE_UNAVAILABLE, "Service Unavailable",
        "Server is overloaded, please retry later");
    return;
  }
  fut.wait();
}

//------------------------------------------------------------------------------
// BDT handlers
void nef_http2_server::handle_bdt_create(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res, bool deprecated) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  auto header_fn = [deprecated](int /*code*/, nlohmann::json& /*resp_body*/) {
    std::map<std::string, std::string> h;
    h["content-type"] = "application/json";
    if (deprecated) h["x-deprecated"] = "true";
    return h;
  };
  // Detached response — return immediately, no fut.wait().
  (void)
      header_fn;  // header logic lives in the adapter sink for the async path
  m_adapter->dispatch_bdt_create_async(
      af_id, json_body, bearer_token, deprecated, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res, bool deprecated) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  auto header_fn = [deprecated](int /*code*/, nlohmann::json& /*resp_body*/) {
    std::map<std::string, std::string> h;
    h["content-type"] = "application/json";
    if (deprecated) h["x-deprecated"] = "true";
    return h;
  };
  // Detached response — return immediately, no fut.wait().
  (void)
      header_fn;  // header logic lives in the adapter sink for the async path
  m_adapter->dispatch_bdt_update_async(
      af_id, bdt_id, json_body, bearer_token, deprecated, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_delete(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& bearer_token, http2_response& res, bool deprecated) {
  std::map<std::string, std::string> h;
  if (deprecated) h["x-deprecated"] = "true";
  // Detached response — return immediately, no fut.wait().
  (void) h;
  m_adapter->dispatch_bdt_delete_async(
      af_id, bdt_id, bearer_token, deprecated, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_get(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& bearer_token, http2_response& res, bool deprecated) {
  auto header_fn = [deprecated](int /*code*/, nlohmann::json& /*resp_body*/) {
    std::map<std::string, std::string> h;
    h["content-type"] = "application/json";
    if (deprecated) h["x-deprecated"] = "true";
    return h;
  };
  dispatch_and_wait_headers(
      [&](response_sink sink) {
        return m_adapter->dispatch_bdt_get(
            af_id, bdt_id, bearer_token, std::move(sink));
      },
      res, header_fn);
}

// QoS handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_create(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // On 201 the app layer stores a relative self-URI in resp_body["self"].
  // The adapter's sink prefixes it with the server address to build the
  // absolute Location header and self field.
  const std::string address = m_address;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_qos_create_async(
      af_id, json_body, bearer_token, address, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_delete(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_qos_delete_async(
      af_id, sub_id, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_get(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_qos_get(
            af_id, sub_id, bearer_token, std::move(sink));
      },
      res);
}

// Analytics handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_create(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_analytics_create(
            af_id, json_body, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_delete(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  dispatch_and_wait_empty(
      [&](response_sink sink) {
        return m_adapter->dispatch_analytics_delete(
            af_id, sub_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_get(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_analytics_get(
            af_id, sub_id, bearer_token, std::move(sink));
      },
      res);
}

// TI PATCH
//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const std::string& patch_body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_patch;
  if (!parse_body_or_400_detail(patch_body, json_patch, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_ti_patch_async(
      af_id, ti_id, json_patch, bearer_token, res.make_deferred());
}

// QoS PATCH
//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_patch(
    const std::string& af_id, const std::string& sub_id,
    const std::string& patch_body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_patch;
  if (!parse_body_or_400_detail(patch_body, json_patch, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_qos_patch_async(
      af_id, sub_id, json_patch, bearer_token, res.make_deferred());
}

// PFD transaction-level and app-level handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_list(
    const std::string& scs_as_id, const std::string& bearer_token,
    http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_pfd_transaction_list(
            scs_as_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_pfd_transaction_put_async(
      scs_as_id, trans_id, json_body, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& bearer_token, http2_response& res) {
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_pfd_transaction_delete_async(
      scs_as_id, trans_id, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& bearer_token,
    http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_pfd_app_get(
            scs_as_id, trans_id, app_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_pfd_app_put_async(
      scs_as_id, trans_id, app_id, json_body, bearer_token,
      res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& patch_body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_patch;
  if (!parse_body_or_400_detail(patch_body, json_patch, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_pfd_app_patch_async(
      scs_as_id, trans_id, app_id, json_patch, bearer_token,
      res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& bearer_token,
    http2_response& res) {
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_pfd_app_delete_async(
      scs_as_id, trans_id, app_id, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_list_transactions(
    const std::string& bearer_token, http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_pfd_list_transactions(
            bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_put_transaction(
    const std::string& trans_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_nnef_pfd_put_transaction_async(
      trans_id, json_body, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_transaction(
    const std::string& trans_id, const std::string& bearer_token,
    http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_pfd_get_transaction(
            trans_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_delete_transaction(
    const std::string& trans_id, const std::string& bearer_token,
    http2_response& res) {
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_nnef_pfd_delete_transaction_async(
      trans_id, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_app(
    const std::string& trans_id, const std::string& app_id,
    const std::string& bearer_token, http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_pfd_get_app(
            trans_id, app_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_put_app(
    const std::string& trans_id, const std::string& app_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_nnef_pfd_put_app_async(
      trans_id, app_id, json_body, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_delete_app(
    const std::string& trans_id, const std::string& app_id,
    const std::string& bearer_token, http2_response& res) {
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_nnef_pfd_delete_app_async(
      trans_id, app_id, bearer_token, res.make_deferred());
}

// Analytics UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_update(
    const std::string& af_id, const std::string& sub_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_analytics_update(
            af_id, sub_id, json_body, bearer_token, std::move(sink));
      },
      res);
}

// Nnef_PFDmanagement extra endpoints
//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter,
    const std::string& bearer_token, http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_pfd_get_applications(
            app_ids_filter, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_partial_pull(
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body;
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    json_body = {};
  }
  // Detached response — return immediately, no fut.wait().
  m_adapter->dispatch_nnef_pfd_partial_pull_async(
      json_body, bearer_token, res.make_deferred());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_create(
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  // On 201 nef_app stamps the new subscription id into resp_body["subId"].
  // The absolute Location header is derived from it, as it always was.
  const std::string address = m_address;
  const std::string sub_path_base =
      nef_sbi_helper::NnefPfdManagementBase +
      nef_config_inst->nef()->get_sbi().get_api_version() +
      nef_sbi_helper::NnefPfdManagementPathSubscriptions + "/";
  auto header_fn = [address, sub_path_base](
                       int http_code, nlohmann::json& resp_body) {
    std::map<std::string, std::string> h;
    h["content-type"] = "application/json";
    if (http_code == http_status_code::CREATED && resp_body.contains("subId") &&
        resp_body["subId"].is_string() &&
        !resp_body["subId"].get<std::string>().empty()) {
      h["location"] =
          address + sub_path_base + resp_body["subId"].get<std::string>();
    }
    return h;
  };
  dispatch_and_wait_headers(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_pfd_subscription_create(
            json_body, bearer_token, std::move(sink));
      },
      res, header_fn);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_get(
    const std::string& sub_id, const std::string& bearer_token,
    http2_response& res) {
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_pfd_subscription_get(
            sub_id, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_put(
    const std::string& sub_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body;
  if (!parse_body_or_400_detail(body, json_body, res)) return;
  dispatch_and_wait(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_pfd_subscription_put(
            sub_id, json_body, bearer_token, std::move(sink));
      },
      res);
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_delete(
    const std::string& sub_id, const std::string& bearer_token,
    http2_response& res) {
  dispatch_and_wait_empty(
      [&](response_sink sink) {
        return m_adapter->dispatch_nnef_pfd_subscription_delete(
            sub_id, bearer_token, std::move(sink));
      },
      res);
}

}  // namespace oai::nef::api
