/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#include "nef-http2-server.h"

#include <cctype>
#include <nlohmann/json.hpp>
#include <string>

#include "3gpp_29.500.h"
#include "logger.hpp"
#include "nef_config.hpp"
#include "nef_health_check.hpp"
#include "nef_rate_limiter.hpp"
#include "nef_request_limits.hpp"

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

using namespace nghttp2::asio_http2;
using namespace nghttp2::asio_http2::server;
using namespace oai::nef::app;

namespace {

//------------------------------------------------------------------------------
static std::string trim_copy(const std::string& in) {
  const auto first = in.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  const auto last = in.find_last_not_of(" \t\r\n");
  return in.substr(first, last - first + 1);
}

//------------------------------------------------------------------------------
static bool starts_with_bearer(const std::string& value) {
  static const std::string kPrefix = "Bearer ";
  if (value.size() < kPrefix.size()) return false;
  for (size_t i = 0; i < kPrefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(kPrefix[i]))) {
      return false;
    }
  }
  return true;
}

//------------------------------------------------------------------------------
static std::string extract_bearer_token(const request& req) {
  const auto& headers = req.header();
  auto it = headers.find("authorization");
  if (it == headers.end()) return "";

  std::string auth_header = trim_copy(it->second.value);
  if (!starts_with_bearer(auth_header)) return "";

  return trim_copy(auth_header.substr(std::string("Bearer ").size()));
}

//------------------------------------------------------------------------------
class request_auth_scope {
 public:
  request_auth_scope(nef_app* nef, const request& req, bool draining = false)
      : m_nef(nef), m_draining(draining) {
    if (m_nef) {
      m_nef->set_request_bearer_token(extract_bearer_token(req));
    }
    // Rate-limit key: use bearer token if present, else remote address fallback.
    const std::string tok = extract_bearer_token(req);
    m_rl_key = tok.empty() ? req.remote_endpoint().address().to_string() : tok;
  }

  ~request_auth_scope() {
    if (m_nef) m_nef->clear_request_bearer_token();
  }

  /// Returns true if the request should be throttled (429) or drained (503).
  bool rate_limited() const {
    return !nef_rate_limiter::instance().allow(m_rl_key);
  }

  /// Returns true if the server is draining (graceful shutdown in progress).
  bool is_draining() const { return m_draining; }

 private:
  nef_app*    m_nef;
  std::string m_rl_key;
  bool        m_draining;
};

//------------------------------------------------------------------------------
static void end_http2_error(
    const response& res, int status, const std::string& title,
    const std::string& detail) {
  auto make_http2_error = [](int status, const std::string& title,
                             const std::string& detail) {
    nlohmann::json pd;
    pd["type"] = "about:blank";
    pd["title"] = title;
    pd["status"] = status;
    pd["detail"] = detail;
    return pd.dump();
  };

  header_map headers;
  headers.insert({"content-type", {"application/problem+json", false}});
  res.write_head(status, headers);
  res.end(make_http2_error(status, title, detail));
}

}  // namespace

//------------------------------------------------------------------------------
// TI GET
void nef_http2_server::handle_ti_get(const std::string& af_id, const std::string& ti_id, const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_traffic_influence_get(af_id, ti_id, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
// TI LIST
void nef_http2_server::handle_ti_list(const std::string& af_id, const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_traffic_influence_list(af_id, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
// Monitoring Event UPDATE (PUT)
void nef_http2_server::handle_monitoring_event_update(const std::string& scs_as_id, const std::string& sub_id, const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_monitoring_event_subscription_update(scs_as_id, sub_id, json_body, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
// QoS UPDATE (PUT)
void nef_http2_server::handle_qos_update(const std::string& af_id, const std::string& sub_id, const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_qos_subscription_update(af_id, sub_id, json_body, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
// BDT PATCH
void nef_http2_server::handle_bdt_patch(const std::string& af_id, const std::string& bdt_id, const std::string& patch_body, const response& res) {
  nlohmann::json json_patch = {};
  try { json_patch = nlohmann::json::parse(patch_body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_bdt_policy_patch(af_id, bdt_id, json_patch, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
// Analytics /fetch
void nef_http2_server::handle_analytics_fetch(const std::string& af_id, const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_analytics_fetch(af_id, json_body, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_subscribe(
    const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }

  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_event_exposure_subscribe(
      json_body, resp_body, http_code, 2);

  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  if (http_code == 201 && resp_body.contains("self") &&
      resp_body["self"].is_string()) {
    headers.insert({"location", {resp_body["self"].get<std::string>(), false}});
  }
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id,
    const response& res) {
  int http_code = 0;
  m_nef_app->handle_nnef_event_exposure_unsubscribe(
      subscription_id, http_code, 2);
  res.write_head(http_code);
  res.end();
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_get(
    const std::string& subscription_id,
    const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_event_exposure_get(
      subscription_id, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_update(
    const std::string& subscription_id,
    const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }

  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_event_exposure_update(
      subscription_id, json_body, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::start() {
  Logger::nef_sbi().info("NEF HTTP/2 server listening on %s:%d",
                         m_address.c_str(), m_port);

  // Nnef_EventExposure /nnef-eventexposure/v1/subscriptions[/{subscriptionId}]
  m_server.handle(
      "/nnef-eventexposure/",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }

          const auto uri = req.uri().path;
          const std::string pfx = "/nnef-eventexposure/v1/";
          const auto pfx_pos = uri.find(pfx);
          if (pfx_pos == std::string::npos) {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
          }

          auto rest = uri.substr(pfx_pos + pfx.size());
          auto s1 = rest.find('/');
          auto resource = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
          auto subscription_id =
              (s1 != std::string::npos) ? rest.substr(s1 + 1) : "";

          if (resource != "subscriptions") {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
          }

          if (req.method() == "POST" && subscription_id.empty()) {
            handle_nnef_event_exposure_subscribe(acc, res);
          } else if (req.method() == "GET" && !subscription_id.empty()) {
            handle_nnef_event_exposure_get(subscription_id, res);
          } else if (req.method() == "PUT" && !subscription_id.empty()) {
            handle_nnef_event_exposure_update(subscription_id, acc, res);
          } else if (req.method() == "DELETE" && !subscription_id.empty()) {
            handle_nnef_event_exposure_unsubscribe(subscription_id, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        });
      });

  // Monitoring Event  /3gpp-monitoring-event/v1/{scsAsId}/subscriptions[/{subId}]
  m_server.handle(
      "/3gpp-monitoring-event/",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }
          auto uri    = req.uri().path;
          // /{base}/{ver}/{scsAsId}/subscriptions[/{subId}]
          const std::string pfx = "/3gpp-monitoring-event/v1/";
          auto rest   = uri.substr(uri.find(pfx) + pfx.size());
          // rest = scsAsId[/subscriptions[/subId]]
          auto s1     = rest.find('/');
          auto scs_as_id = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
          auto after_scs = (s1 != std::string::npos) ? rest.substr(s1+1) : "";
          // after_scs = "subscriptions" | "subscriptions/<subId>"
          auto s2     = after_scs.find('/');
          auto sub_id = (s2 != std::string::npos) ? after_scs.substr(s2+1) : "";

          if (req.method() == "POST") {
            handle_monitoring_event_subscribe(scs_as_id, acc, res);
          } else if (req.method() == "DELETE" && !sub_id.empty()) {
            handle_monitoring_event_unsubscribe(scs_as_id, sub_id, res);
          } else if (req.method() == "PUT" && !sub_id.empty()) {
            handle_monitoring_event_update(scs_as_id, sub_id, acc, res);
          } else if (req.method() == "GET") {
            handle_monitoring_event_get(scs_as_id, sub_id, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        });
      });

  // Traffic Influence  /3gpp-traffic-influence/v1/{afId}/subscriptions[/{appSessionId}]
  m_server.handle(
      "/3gpp-traffic-influence/",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }
          auto uri  = req.uri().path;
          const std::string pfx = "/3gpp-traffic-influence/v1/";
          auto rest = uri.substr(uri.find(pfx) + pfx.size());
          auto s1   = rest.find('/');
          auto af_id   = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
          auto after   = (s1 != std::string::npos) ? rest.substr(s1+1) : "";
          // after = "subscriptions" | "subscriptions/<appSessionId>"
          auto s2    = after.find('/');
          std::string sub_path = (s2 != std::string::npos) ? after.substr(0, s2) : after;
          std::string app_session_id = (s2 != std::string::npos) ? after.substr(s2+1) : "";

          if (sub_path == "subscriptions") {
            if (req.method() == "GET" && app_session_id.empty()) {
              handle_ti_list(af_id, res);
            } else if (req.method() == "GET" && !app_session_id.empty()) {
              handle_ti_get(af_id, app_session_id, res);
            } else if (req.method() == "POST" && app_session_id.empty()) {
              handle_ti_create(af_id, acc, res);
            } else if (req.method() == "PUT" && !app_session_id.empty()) {
              handle_ti_update(af_id, app_session_id, acc, res);
            } else if (req.method() == "PATCH" && !app_session_id.empty()) {
              handle_ti_patch(af_id, app_session_id, acc, res);
            } else if (req.method() == "DELETE" && !app_session_id.empty()) {
              handle_ti_delete(af_id, app_session_id, res);
            } else {
              end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
            }
          } else {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found");
          }
        });
      });

  // PFD Management  /3gpp-pfd-management/v1/{scsAsId}/transactions[/{transId}[/applications/{appId}]]
  m_server.handle(
      "/3gpp-pfd-management/",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }
          auto uri  = req.uri().path;
          const std::string pfx = "/3gpp-pfd-management/v1/";
          auto rest = uri.substr(uri.find(pfx) + pfx.size());
          // rest = {scsAsId}/transactions[/{transId}[/applications/{appId}]]
          auto s1 = rest.find('/');
          auto scs_as_id = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
          auto after_scs = (s1 != std::string::npos) ? rest.substr(s1 + 1) : "";
          // after_scs = "transactions" | "transactions/{transId}" | "transactions/{transId}/applications/{appId}"
          auto s2 = after_scs.find('/');
          auto top_seg = (s2 != std::string::npos) ? after_scs.substr(0, s2) : after_scs;
          auto after_trans_key = (s2 != std::string::npos) ? after_scs.substr(s2 + 1) : "";
          // top_seg should be "transactions"
          auto s3 = after_trans_key.find('/');
          auto trans_id = (s3 != std::string::npos) ? after_trans_key.substr(0, s3) : after_trans_key;
          auto after_trans_id = (s3 != std::string::npos) ? after_trans_key.substr(s3 + 1) : "";
          // after_trans_id = "applications/{appId}" or empty
          std::string app_id;
          const std::string apps_pfx = "applications/";
          if (after_trans_id.find(apps_pfx) == 0) {
            app_id = after_trans_id.substr(apps_pfx.size());
          }

          if (top_seg != "transactions") {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
          }

          if (trans_id.empty()) {
            // /transactions  → list
            if (req.method() == "GET") {
              handle_pfd_transaction_list(scs_as_id, res);
            } else {
              end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
            }
          } else if (app_id.empty()) {
            // /transactions/{transId}
            if (req.method() == "PUT") {
              handle_pfd_transaction_put(scs_as_id, trans_id, acc, res);
            } else if (req.method() == "DELETE") {
              handle_pfd_transaction_delete(scs_as_id, trans_id, res);
            } else if (req.method() == "GET") {
              // Return transaction body via pfd_get (legacy app-level)
              nlohmann::json resp_body; int http_code = 0;
              m_nef_app->handle_pfd_transaction_list(scs_as_id, resp_body, http_code, 2);
              header_map h; h.insert({"content-type", {"application/json", false}});
              res.write_head(http_code, h); res.end(resp_body.dump());
            } else {
              end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
            }
          } else {
            // /transactions/{transId}/applications/{appId}
            if (req.method() == "GET") {
              handle_pfd_app_get(scs_as_id, trans_id, app_id, res);
            } else if (req.method() == "PUT") {
              handle_pfd_app_put(scs_as_id, trans_id, app_id, acc, res);
            } else if (req.method() == "PATCH") {
              handle_pfd_app_patch(scs_as_id, trans_id, app_id, acc, res);
            } else if (req.method() == "DELETE") {
              handle_pfd_app_delete(scs_as_id, trans_id, app_id, res);
            } else {
              end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
            }
          }
        });
      });

  // Nnef_PFDmanagement /nnef-pfdmanagement/v1/transactions[/{transId}[/applications/{appId}]]
  m_server.handle(
      "/nnef-pfdmanagement/",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }

          const auto uri = req.uri().path;
          const std::string pfx = "/nnef-pfdmanagement/v1/";
          const auto pfx_pos = uri.find(pfx);
          if (pfx_pos == std::string::npos) {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
          }

          auto rest = uri.substr(pfx_pos + pfx.size());
          auto s1 = rest.find('/');
          const auto resource = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
          const auto after_resource =
              (s1 != std::string::npos) ? rest.substr(s1 + 1) : "";

          if (resource != "transactions") {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
          }

          if (after_resource.empty()) {
            if (req.method() == "GET") {
              handle_nnef_pfd_list_transactions(res);
            } else {
              end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
            }
            return;
          }

          auto s2 = after_resource.find('/');
          const auto trans_id =
              (s2 != std::string::npos) ? after_resource.substr(0, s2) : after_resource;
          const auto after_trans_id =
              (s2 != std::string::npos) ? after_resource.substr(s2 + 1) : "";

          if (trans_id.empty()) {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
          }

          if (after_trans_id.empty()) {
            if (req.method() == "GET") {
              handle_nnef_pfd_get_transaction(trans_id, res);
            } else if (req.method() == "PUT") {
              handle_nnef_pfd_put_transaction(trans_id, acc, res);
            } else if (req.method() == "DELETE") {
              handle_nnef_pfd_delete_transaction(trans_id, res);
            } else {
              end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
            }
            return;
          }

          const std::string apps_pfx = "applications/";
          if (after_trans_id.find(apps_pfx) != 0) {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
          }

          const auto app_id = after_trans_id.substr(apps_pfx.size());
          if (app_id.empty()) {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
          }

          if (req.method() == "GET") {
            handle_nnef_pfd_get_app(trans_id, app_id, res);
          } else if (req.method() == "PUT") {
            handle_nnef_pfd_put_app(trans_id, app_id, acc, res);
          } else if (req.method() == "DELETE") {
            handle_nnef_pfd_delete_app(trans_id, app_id, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        });
      });

  // Nnef_PFDmanagement — /nnef-pfdmanagement/v1/applications[/partial-pull]
  m_server.handle(
      "/nnef-pfdmanagement/v1/applications",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            end_http2_error(res, 413, "Payload Too Large",
                            "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }

          const auto uri  = req.uri().path;
          const auto qstr = req.uri().raw_query;  // e.g. "app-ids=app1&app-ids=app2"

          // Check for /partial-pull suffix
          if (uri.size() >= 13 &&
              uri.substr(uri.size() - 13) == "/partial-pull") {
            if (req.method() == "POST") {
              handle_nnef_pfd_partial_pull(acc, res);
            } else {
              end_http2_error(res, 405, "Method Not Allowed",
                              "HTTP method is not supported");
            }
            return;
          }

          if (req.method() == "GET") {
            // Parse app-ids query param (repeated: ?app-ids=a&app-ids=b)
            std::vector<std::string> ids;
            std::string q = qstr;
            std::string token;
            while (!q.empty()) {
              auto amp = q.find('&');
              token = (amp != std::string::npos) ? q.substr(0, amp) : q;
              q     = (amp != std::string::npos) ? q.substr(amp + 1) : "";
              const std::string key = "app-ids=";
              if (token.substr(0, key.size()) == key)
                ids.push_back(token.substr(key.size()));
            }
            handle_nnef_pfd_get_applications(ids, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed",
                            "HTTP method is not supported");
          }
        });
      });

  // Nnef_PFDmanagement — /nnef-pfdmanagement/v1/subscriptions[/{subId}]
  m_server.handle(
      "/nnef-pfdmanagement/v1/subscriptions",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            end_http2_error(res, 413, "Payload Too Large",
                            "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }

          const auto uri = req.uri().path;
          const std::string pfx = "/nnef-pfdmanagement/v1/subscriptions";
          auto rest = uri.substr(pfx.size());
          // rest is "" or "/<subId>"
          if (rest.empty() || rest == "/") {
            if (req.method() == "POST") {
              handle_nnef_pfd_subscription_create(acc, res);
            } else {
              end_http2_error(res, 405, "Method Not Allowed",
                              "HTTP method is not supported");
            }
            return;
          }
          // sub ID follows "/"
          const auto sub_id = (rest[0] == '/') ? rest.substr(1) : rest;
          if (sub_id.empty()) {
            end_http2_error(res, 404, "Not Found",
                            "Requested resource was not found");
            return;
          }
          if (req.method() == "GET") {
            handle_nnef_pfd_subscription_get(sub_id, res);
          } else if (req.method() == "PUT") {
            handle_nnef_pfd_subscription_put(sub_id, acc, res);
          } else if (req.method() == "DELETE") {
            handle_nnef_pfd_subscription_delete(sub_id, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed",
                            "HTTP method is not supported");
          }
        });
      });

  // BDT  /3gpp-bdt/v1/{scsAsId}/policies[/{polId}]
  m_server.handle(
      "/3gpp-bdt/",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }
          auto uri = req.uri().path;
          const std::string pfx = "/3gpp-bdt/v1/";
          auto rest  = uri.substr(uri.find(pfx) + pfx.size());
          auto s1    = rest.find('/');
          auto af_id = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
          auto after = (s1 != std::string::npos) ? rest.substr(s1+1) : "";
          // after = "policies" | "policies/<polId>" | "bdtPolicies/<polId>"
          auto s2    = after.find('/');
          auto policy_path = (s2 != std::string::npos) ? after.substr(0, s2) : after;
          auto pol_id = (s2 != std::string::npos) ? after.substr(s2+1) : "";

          // F1.9: detect legacy /policies path and emit deprecation warning
          bool legacy_path = (policy_path == "policies");
          if (legacy_path) {
            Logger::nef_sbi().warn(
                "HTTP/2: BDT request on deprecated path '/policies' (af_id='%s'); "
                "please migrate to canonical '/bdtPolicies' path (TS 29.122 §5.13)",
                af_id.c_str());
          }

          if (req.method() == "PATCH" && policy_path == "bdtPolicies" && !pol_id.empty()) {
            handle_bdt_patch(af_id, pol_id, acc, res);
          } else if (req.method() == "POST") {
            handle_bdt_create(af_id, acc, res, legacy_path);
          } else if (req.method() == "GET" && pol_id.empty()) {
            handle_bdt_get(af_id, "", res, legacy_path);  // list
          } else if (req.method() == "GET" && !pol_id.empty()) {
            handle_bdt_get(af_id, pol_id, res, legacy_path);
          } else if (req.method() == "PUT" && !pol_id.empty()) {
            handle_bdt_update(af_id, pol_id, acc, res, legacy_path);
          } else if (req.method() == "DELETE" && !pol_id.empty()) {
            handle_bdt_delete(af_id, pol_id, res, legacy_path);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        });
      });

  // QoS  /3gpp-as-session-with-qos/v1/{afId}/subscriptions[/{subId}]
  m_server.handle(
      "/3gpp-as-session-with-qos/",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }
          auto uri = req.uri().path;
          const std::string pfx = "/3gpp-as-session-with-qos/v1/";
          auto rest   = uri.substr(uri.find(pfx) + pfx.size());
          auto s1     = rest.find('/');
          auto af_id  = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
          auto after  = (s1 != std::string::npos) ? rest.substr(s1+1) : "";
          auto s2     = after.find('/');
          std::string sub_path = (s2 != std::string::npos) ? after.substr(0, s2) : after;
          std::string sub_id = (s2 != std::string::npos) ? after.substr(s2+1) : "";

          if (sub_path == "subscriptions") {
            if (req.method() == "POST" && sub_id.empty()) {
              handle_qos_create(af_id, acc, res);
            } else if (req.method() == "GET" && sub_id.empty()) {
              handle_qos_get(af_id, "", res);  // list
            } else if (req.method() == "GET" && !sub_id.empty()) {
              handle_qos_get(af_id, sub_id, res);
            } else if (req.method() == "PUT" && !sub_id.empty()) {
              handle_qos_update(af_id, sub_id, acc, res);
            } else if (req.method() == "PATCH" && !sub_id.empty()) {
              handle_qos_patch(af_id, sub_id, acc, res);
            } else if (req.method() == "DELETE" && !sub_id.empty()) {
              handle_qos_delete(af_id, sub_id, res);
            } else {
              end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
            }
          } else {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found");
          }
        });
      });
  // Monitoring Event UPDATE (PUT) /3gpp-monitoring-event/v1/{scsAsId}/subscriptions/{subId}
  // (Extend existing handler)
  // Already handled in the main /3gpp-monitoring-event/ route, add PUT support:
  // (No code needed here, just ensure handler is implemented below)
  // Analytics /fetch endpoint
  m_server.handle(
      "/3gpp-analyticsexposure/",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }
          auto uri = req.uri().path;
          const std::string pfx = "/3gpp-analyticsexposure/v1/";
          auto rest = uri.substr(uri.find(pfx) + pfx.size());
          auto s1 = rest.find('/');
          auto af_id = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
          auto after = (s1 != std::string::npos) ? rest.substr(s1+1) : "";
          if (after == "fetch" && req.method() == "POST") {
            handle_analytics_fetch(af_id, acc, res);
          } else {
            end_http2_error(res, 404, "Not Found", "Requested resource was not found");
          }
        });
      });

  // Analytics  /3gpp-analyticsexposure/v1/{afId}/subscriptions[/{subId}]
  m_server.handle(
      "/3gpp-analyticsexposure/",
      [&](const request& req, const response& res) {
        std::string acc;
        req.on_data([&req, &res, &acc, this](
                        const uint8_t* data, std::size_t len) {
          if (acc.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(acc.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            acc.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          acc.append(reinterpret_cast<const char*>(data), len);
          if (len != 0) return;
          request_auth_scope auth_scope(m_nef_app, req, m_draining.load(std::memory_order_relaxed));
          if (auth_scope.is_draining()) { end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return; }
          if (auth_scope.rate_limited()) { end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return; }
          auto uri = req.uri().path;
          const std::string pfx = "/3gpp-analyticsexposure/v1/";
          auto rest   = uri.substr(uri.find(pfx) + pfx.size());
          auto s1     = rest.find('/');
          auto af_id  = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
          auto after  = (s1 != std::string::npos) ? rest.substr(s1+1) : "";
          auto s2     = after.find('/');
          auto sub_id = (s2 != std::string::npos) ? after.substr(s2+1) : "";

          if (req.method() == "POST") {
            handle_analytics_create(af_id, acc, res);
          } else if (req.method() == "GET" && sub_id.empty()) {
            handle_analytics_get(af_id, "", res);  // list
          } else if (req.method() == "GET" && !sub_id.empty()) {
            handle_analytics_get(af_id, sub_id, res);
          } else if (req.method() == "PUT" && !sub_id.empty()) {
            handle_analytics_update(af_id, sub_id, acc, res);
          } else if (req.method() == "DELETE" && !sub_id.empty()) {
            handle_analytics_delete(af_id, sub_id, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        });
      });

  // Inbound NF notification receive endpoint
  // AMF/SMF/PCF POST to: /nef-notify/v1/notify/{nf_sub_id}
  // NEF translates and forwards to the AF's notificationDestination.
  m_server.handle(
      "/nef-notify/",
      [&](const request& req, const response& res) {
        std::string accumulated;
        req.on_data([&req, &res, &accumulated, this](
                        const uint8_t* data, std::size_t len) {
          if (accumulated.size() > nef_request_limits::MAX_REQUEST_BODY_BYTES) return;
          if (len > 0 && nef_request_limits::is_body_too_large(accumulated.size(), len)) {
            Logger::nef_sbi().warn(
                "HTTP/2: request body too large (limit 1 MiB), returning 413");
            end_http2_error(res, 413, "Payload Too Large", "Request body exceeds maximum allowed size");
            accumulated.resize(nef_request_limits::MAX_REQUEST_BODY_BYTES + 1);
            return;
          }
          accumulated.append(reinterpret_cast<const char*>(data), len);
          if (len == 0) {
            request_auth_scope auth_scope(
                m_nef_app, req, m_draining.load(std::memory_order_relaxed));
            if (auth_scope.is_draining()) {
              end_http2_error(
                  res, 503, "Service Unavailable", "Server is shutting down");
              return;
            }
            // End of data – dispatch
            if (req.method() == "POST") {
              // Extract nf_sub_id from URI:
              //   /nef-notify/v1/notify/<nf_sub_id>
              auto uri = req.uri().path;
              const std::string prefix = "/nef-notify/v1/notify/";
              std::string nf_sub_id;
              auto pos = uri.find(prefix);
              if (pos != std::string::npos) {
                nf_sub_id = uri.substr(pos + prefix.size());
                // Remove trailing slashes
                while (!nf_sub_id.empty() && nf_sub_id.back() == '/')
                  nf_sub_id.pop_back();
              }
              Logger::nef_sbi().debug(
                  "Received NF notification for sub-id: %s",
                  nf_sub_id.c_str());
              handle_nf_notify(nf_sub_id, accumulated, res);
            } else {
              end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
            }
          }
        });
      });

  // Health check endpoint
  // GET /healthz  → 200 {"status":"ok", ...}  or  503 {"status":"draining",...}
  m_server.handle(
      "/healthz",
      [&](const request& req, const response& res) {
        req.on_data([&req, &res, this](const uint8_t*, std::size_t len) {
          if (len != 0) return;  // wait for end-of-body signal
          if (req.method() != "GET") {
            end_http2_error(
                res, 405, "Method Not Allowed",
                "HTTP method is not supported for this resource");
            return;
          }
          const bool draining = m_draining.load(std::memory_order_relaxed);
          const int uptime = static_cast<int>(
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now() - m_start_time)
                  .count());
          const std::string instance_id =
              m_nef_app ? m_nef_app->get_nef_instance_id() : "";
          int http_code = 0;
          const std::string body =
              oai::nef::app::nef_health_check::make_response(
                  draining, instance_id, uptime, http_code);
          header_map headers;
          headers.insert({"content-type", {"application/json", false}});
          res.write_head(http_code, headers);
          res.end(body);
        });
      });

  boost::system::error_code ec;
  if (m_server.listen_and_serve(
          ec, m_address, std::to_string(m_port), true)) {
    Logger::nef_sbi().error(
        "HTTP/2 server error: %s", ec.message().c_str());
    return;
  }
  m_running = true;
  m_server.io_services().front()->run();
}

//------------------------------------------------------------------------------
void nef_http2_server::stop() {
  m_server.stop();
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
    const std::string& scs_as_id,
    const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }

  std::string    sub_id;
  nlohmann::json resp_body;
  int            http_code = 0;

  m_nef_app->handle_monitoring_event_subscription_create(
      scs_as_id, json_body, sub_id, resp_body, http_code, 2);

  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_monitoring_event_unsubscribe(
    const std::string& scs_as_id,
    const std::string& sub_id,
    const response& res) {
  int http_code = 0;
  m_nef_app->handle_monitoring_event_subscription_delete(
      scs_as_id, sub_id, http_code, 2);
  res.write_head(http_code);
  res.end();
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_monitoring_event_get(
    const std::string& scs_as_id,
    const std::string& sub_id,
    const response& res) {
  nlohmann::json resp_body;
  int            http_code = 0;
  m_nef_app->handle_monitoring_event_subscription_get(
      scs_as_id, sub_id, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_create(
    const std::string& af_id,
    const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  std::string ti_id;
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_traffic_influence_create(
      af_id, json_body, ti_id, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_update(
    const std::string& af_id,
    const std::string& ti_id,
    const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_traffic_influence_update(
      af_id, ti_id, json_body, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_delete(
    const std::string& af_id,
    const std::string& ti_id,
    const response& res) {
  int http_code = 0;
  m_nef_app->handle_traffic_influence_delete(af_id, ti_id, http_code, 2);
  res.write_head(http_code); res.end();
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_create(
    const std::string& app_id,
    const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_pfd_create(app_id, json_body, resp_body, http_code, 2);
  header_map headers;
  headers.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, headers);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_delete(
    const std::string& app_id, const response& res) {
  int http_code = 0;
  m_nef_app->handle_pfd_delete(app_id, http_code, 2);
  res.write_head(http_code); res.end();
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nf_notify(
    const std::string& nf_sub_id,
    const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  if (!body.empty()) {
    try {
      json_body = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
      Logger::nef_sbi().warn(
          "Failed to parse NF notification body: %s", e.what());
      end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
    }
  }

  // Delegate to nef_app which looks up nf_sub_id → af_sub_id and forwards
  m_nef_app->handle_nf_notification(nf_sub_id, json_body);

  // Acknowledge to the NF
  res.write_head(204);
  res.end();
}

//------------------------------------------------------------------------------
// BDT handlers
void nef_http2_server::handle_bdt_create(
    const std::string& af_id, const std::string& body, const response& res,
    bool deprecated) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  std::string bdt_id;
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_bdt_policy_create(af_id, json_body, bdt_id, resp_body,
                                       http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  if (deprecated) h.insert({"x-deprecated", {"true", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& body, const response& res, bool deprecated) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_bdt_policy_update(af_id, bdt_id, json_body, resp_body,
                                       http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  if (deprecated) h.insert({"x-deprecated", {"true", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_delete(
    const std::string& af_id, const std::string& bdt_id,
    const response& res, bool deprecated) {
  int http_code = 0;
  m_nef_app->handle_bdt_policy_delete(af_id, bdt_id, http_code, 2);
  if (deprecated) {
    header_map h; h.insert({"x-deprecated", {"true", false}});
    res.write_head(http_code, h); res.end();
  } else {
    res.write_head(http_code); res.end();
  }
}

//------------------------------------------------------------------------------void nef_http2_server::handle_bdt_get(
    const std::string& af_id, const std::string& bdt_id,
    const response& res, bool deprecated) {
  nlohmann::json resp_body;
  int http_code = 0;
  if (bdt_id.empty()) {
    m_nef_app->handle_bdt_policy_list(af_id, resp_body, http_code, 2);
  } else {
    m_nef_app->handle_bdt_policy_get(af_id, bdt_id, resp_body, http_code, 2);
  }
  header_map h; h.insert({"content-type", {"application/json", false}});
  if (deprecated) h.insert({"x-deprecated", {"true", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

// QoS handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_create(
    const std::string& af_id, const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  std::string sub_id;
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_qos_subscription_create(af_id, json_body, sub_id,
                                             resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_delete(
    const std::string& af_id, const std::string& sub_id,
    const response& res) {
  int http_code = 0;
  m_nef_app->handle_qos_subscription_delete(af_id, sub_id, http_code, 2);
  res.write_head(http_code); res.end();
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_get(
    const std::string& af_id, const std::string& sub_id,
    const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  if (sub_id.empty()) {
    m_nef_app->handle_qos_subscription_list(af_id, resp_body, http_code, 2);
  } else {
    m_nef_app->handle_qos_subscription_get(af_id, sub_id, resp_body,
                                            http_code, 2);
  }
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

// Analytics handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_create(
    const std::string& af_id, const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  std::string sub_id;
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_analytics_subscription_create(af_id, json_body, sub_id,
                                                   resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_delete(
    const std::string& af_id, const std::string& sub_id,
    const response& res) {
  int http_code = 0;
  m_nef_app->handle_analytics_subscription_delete(af_id, sub_id, http_code, 2);
  res.write_head(http_code); res.end();
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_get(
    const std::string& af_id, const std::string& sub_id,
    const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  if (sub_id.empty()) {
    m_nef_app->handle_analytics_subscription_list(af_id, resp_body,
                                                   http_code, 2);
  } else {
    m_nef_app->handle_analytics_subscription_get(af_id, sub_id, resp_body,
                                                  http_code, 2);
  }
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

// TI PATCH
//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const std::string& patch_body, const response& res) {
  nlohmann::json json_patch = {};
  try { json_patch = nlohmann::json::parse(patch_body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_traffic_influence_patch(af_id, ti_id, json_patch, resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

// QoS PATCH
//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_patch(
    const std::string& af_id, const std::string& sub_id,
    const std::string& patch_body, const response& res) {
  nlohmann::json json_patch = {};
  try { json_patch = nlohmann::json::parse(patch_body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_qos_subscription_patch(af_id, sub_id, json_patch, resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

// PFD transaction-level and app-level handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_list(
    const std::string& scs_as_id, const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_pfd_transaction_list(scs_as_id, resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_pfd_transaction_put(scs_as_id, trans_id, json_body, resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const response& res) {
  int http_code = 0;
  m_nef_app->handle_pfd_transaction_delete(scs_as_id, trans_id, http_code, 2);
  res.write_head(http_code); res.end();
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_pfd_app_get(scs_as_id, trans_id, app_id, resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_pfd_app_put(scs_as_id, trans_id, app_id, json_body, resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& patch_body,
    const response& res) {
  nlohmann::json json_patch = {};
  try { json_patch = nlohmann::json::parse(patch_body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_pfd_app_patch(scs_as_id, trans_id, app_id, json_patch, resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const response& res) {
  int http_code = 0;
  m_nef_app->handle_pfd_app_delete(scs_as_id, trans_id, app_id, http_code, 2);
  res.write_head(http_code); res.end();
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_list_transactions(
    const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_list_transactions(resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_put_transaction(
    const std::string& trans_id,
    const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_put_transaction(
      trans_id, json_body, resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_transaction(
    const std::string& trans_id,
    const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_get_transaction(
      trans_id, resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_delete_transaction(
    const std::string& trans_id,
    const response& res) {
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_delete_transaction(trans_id, http_code, 2);
  res.write_head(http_code);
  res.end();
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_app(
    const std::string& trans_id,
    const std::string& app_id,
    const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_get_app(
      trans_id, app_id, resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_put_app(
    const std::string& trans_id,
    const std::string& app_id,
    const std::string& body,
    const response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_put_app(
      trans_id, app_id, json_body, resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_delete_app(
    const std::string& trans_id,
    const std::string& app_id,
    const response& res) {
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_delete_app(trans_id, app_id, http_code, 2);
  res.write_head(http_code);
  res.end();
}

// Analytics UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_update(
    const std::string& af_id, const std::string& sub_id,
    const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_analytics_subscription_update(af_id, sub_id, json_body, resp_body, http_code, 2);
  header_map h; h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h); res.end(resp_body.dump());
}

// Nnef_PFDmanagement extra endpoints
//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter, const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_get_applications(app_ids_filter, resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_partial_pull(
    const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) { json_body = {}; }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_partial_pull(json_body, resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_create(
    const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  std::string sub_id;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_subscription_create(
      json_body, sub_id, resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  if (http_code == 201 && !sub_id.empty()) {
    const std::string loc =
        m_address + "/nnef-pfdmanagement/v1/subscriptions/" + sub_id;
    h.insert({"location", {loc, false}});
  }
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_get(
    const std::string& sub_id, const response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_subscription_get(sub_id, resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_put(
    const std::string& sub_id, const std::string& body, const response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_subscription_put(sub_id, json_body, resp_body, http_code, 2);
  header_map h;
  h.insert({"content-type", {"application/json", false}});
  res.write_head(http_code, h);
  res.end(resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_delete(
    const std::string& sub_id, const response& res) {
  int http_code = 0;
  m_nef_app->handle_nnef_pfd_subscription_delete(sub_id, http_code, 2);
  res.write_head(http_code);
  res.end();
}

