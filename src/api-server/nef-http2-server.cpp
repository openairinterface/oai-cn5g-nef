/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
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

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

using namespace oai::nef::app;

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

// Inline bearer-token extraction helper (used in route lambdas)
// Extracts "Bearer <token>" from the Authorization header, case-insensitively
// matching the "Bearer " prefix.
static std::string extract_bearer(const http2_request& req) {
  auto it = req.headers.find("authorization");
  if (it == req.headers.end()) return "";
  const std::string& auth = it->second;
  // Check for "Bearer " prefix (case-insensitive for the keyword)
  static const std::string kBearer = "Bearer ";
  if (auth.size() <= kBearer.size()) return "";
  for (size_t i = 0; i < kBearer.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(auth[i])) !=
        std::tolower(static_cast<unsigned char>(kBearer[i])))
      return "";
  }
  return auth.substr(kBearer.size());
}

}  // namespace

//------------------------------------------------------------------------------
// TI GET
void nef_http2_server::handle_ti_get(const std::string& af_id, const std::string& ti_id, const std::string& bearer_token, http2_response& res) {
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_traffic_influence_get(af_id, ti_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
// TI LIST
void nef_http2_server::handle_ti_list(const std::string& af_id, const std::string& bearer_token, http2_response& res) {
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_traffic_influence_list(af_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
// Monitoring Event UPDATE (PUT)
void nef_http2_server::handle_monitoring_event_update(const std::string& scs_as_id, const std::string& sub_id, const std::string& body, const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_monitoring_event_subscription_update(scs_as_id, sub_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
// QoS UPDATE (PUT)
void nef_http2_server::handle_qos_update(const std::string& af_id, const std::string& sub_id, const std::string& body, const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_qos_subscription_update(af_id, sub_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
// BDT PATCH
void nef_http2_server::handle_bdt_patch(const std::string& af_id, const std::string& bdt_id, const std::string& patch_body, const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_patch = {};
  try { json_patch = nlohmann::json::parse(patch_body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_bdt_policy_patch(af_id, bdt_id, json_patch, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
// Analytics /fetch
void nef_http2_server::handle_analytics_fetch(const std::string& af_id, const std::string& body, const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_analytics_fetch(af_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_subscribe(
    const std::string& body,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }

  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_event_exposure_subscribe(
      json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();

  std::map<std::string, std::string> headers;
  headers["content-type"] = "application/json";
  if (http_code == 201 && resp_body.contains("self") &&
      resp_body["self"].is_string()) {
    headers["location"] = resp_body["self"].get<std::string>();
  }
  res.send(http_code, headers, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id,
    const std::string& bearer_token,
    http2_response& res) {
  m_nef_app->set_request_bearer_token(bearer_token);
  int http_code = 0;
  m_nef_app->handle_nnef_event_exposure_unsubscribe(
      subscription_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {});
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_get(
    const std::string& subscription_id,
    const std::string& bearer_token,
    http2_response& res) {
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_event_exposure_get(
      subscription_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_update(
    const std::string& subscription_id,
    const std::string& body,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }

  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_nnef_event_exposure_update(
      subscription_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::start() {
  Logger::nef_sbi().info("NEF HTTP/2 server listening on {}:{}",
                         m_address, m_port);

  // Nnef_EventExposure /nnef-eventexposure/v1/subscriptions[/{subscriptionId}]
  server_.handle(
      "/nnef-eventexposure/",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }

        const std::string pfx = "/nnef-eventexposure/v1/";
        const auto pfx_pos = req.path.find(pfx);
        if (pfx_pos == std::string::npos) {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
        }

        auto rest = req.path.substr(pfx_pos + pfx.size());
        auto s1 = rest.find('/');
        auto resource = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
        auto subscription_id = (s1 != std::string::npos) ? rest.substr(s1 + 1) : "";

        if (resource != "subscriptions") {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
        }

        if (req.method == "POST" && subscription_id.empty()) {
          handle_nnef_event_exposure_subscribe(req.body, bearer_token, res);
        } else if (req.method == "GET" && !subscription_id.empty()) {
          handle_nnef_event_exposure_get(subscription_id, bearer_token, res);
        } else if (req.method == "PUT" && !subscription_id.empty()) {
          handle_nnef_event_exposure_update(subscription_id, req.body, bearer_token, res);
        } else if (req.method == "DELETE" && !subscription_id.empty()) {
          handle_nnef_event_exposure_unsubscribe(subscription_id, bearer_token, res);
        } else {
          end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
        }
      });

  // Monitoring Event  /3gpp-monitoring-event/v1/{scsAsId}/subscriptions[/{subId}]
  server_.handle(
      "/3gpp-monitoring-event/",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        // /{base}/{ver}/{scsAsId}/subscriptions[/{subId}]
        const std::string pfx = "/3gpp-monitoring-event/v1/";
        auto rest   = req.path.substr(req.path.find(pfx) + pfx.size());
        // rest = scsAsId[/subscriptions[/subId]]
        auto s1     = rest.find('/');
        auto scs_as_id = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
        auto after_scs = (s1 != std::string::npos) ? rest.substr(s1+1) : "";
        // after_scs = "subscriptions" | "subscriptions/<subId>"
        auto s2     = after_scs.find('/');
        auto sub_id = (s2 != std::string::npos) ? after_scs.substr(s2+1) : "";

        if (req.method == "POST") {
          handle_monitoring_event_subscribe(scs_as_id, req.body, bearer_token, res);
        } else if (req.method == "DELETE" && !sub_id.empty()) {
          handle_monitoring_event_unsubscribe(scs_as_id, sub_id, bearer_token, res);
        } else if (req.method == "PUT" && !sub_id.empty()) {
          handle_monitoring_event_update(scs_as_id, sub_id, req.body, bearer_token, res);
        } else if (req.method == "GET") {
          handle_monitoring_event_get(scs_as_id, sub_id, bearer_token, res);
        } else {
          end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
        }
      });

  // Traffic Influence  /3gpp-traffic-influence/v1/{afId}/subscriptions[/{appSessionId}]
  server_.handle(
      "/3gpp-traffic-influence/",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        const std::string pfx = "/3gpp-traffic-influence/v1/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
        auto s1   = rest.find('/');
        auto af_id   = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
        auto after   = (s1 != std::string::npos) ? rest.substr(s1+1) : "";
        // after = "subscriptions" | "subscriptions/<appSessionId>"
        auto s2    = after.find('/');
        std::string sub_path = (s2 != std::string::npos) ? after.substr(0, s2) : after;
        std::string app_session_id = (s2 != std::string::npos) ? after.substr(s2+1) : "";

        if (sub_path == "subscriptions") {
          if (req.method == "GET" && app_session_id.empty()) {
            handle_ti_list(af_id, bearer_token, res);
          } else if (req.method == "GET" && !app_session_id.empty()) {
            handle_ti_get(af_id, app_session_id, bearer_token, res);
          } else if (req.method == "POST" && app_session_id.empty()) {
            handle_ti_create(af_id, req.body, bearer_token, res);
          } else if (req.method == "PUT" && !app_session_id.empty()) {
            handle_ti_update(af_id, app_session_id, req.body, bearer_token, res);
          } else if (req.method == "PATCH" && !app_session_id.empty()) {
            handle_ti_patch(af_id, app_session_id, req.body, bearer_token, res);
          } else if (req.method == "DELETE" && !app_session_id.empty()) {
            handle_ti_delete(af_id, app_session_id, bearer_token, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        } else {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found");
        }
      });

  // PFD Management  /3gpp-pfd-management/v1/{scsAsId}/transactions[/{transId}[/applications/{appId}]]
  server_.handle(
      "/3gpp-pfd-management/",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        const std::string pfx = "/3gpp-pfd-management/v1/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
        // rest = {scsAsId}/transactions[/{transId}[/applications/{appId}]]
        auto s1 = rest.find('/');
        auto scs_as_id = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
        auto after_scs = (s1 != std::string::npos) ? rest.substr(s1 + 1) : "";
        auto s2 = after_scs.find('/');
        auto top_seg = (s2 != std::string::npos) ? after_scs.substr(0, s2) : after_scs;
        auto after_trans_key = (s2 != std::string::npos) ? after_scs.substr(s2 + 1) : "";
        auto s3 = after_trans_key.find('/');
        auto trans_id = (s3 != std::string::npos) ? after_trans_key.substr(0, s3) : after_trans_key;
        auto after_trans_id = (s3 != std::string::npos) ? after_trans_key.substr(s3 + 1) : "";
        std::string app_id;
        const std::string apps_pfx = "applications/";
        if (after_trans_id.find(apps_pfx) == 0) {
          app_id = after_trans_id.substr(apps_pfx.size());
        }

        if (top_seg != "transactions") {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
        }

        if (trans_id.empty()) {
          if (req.method == "GET") {
            handle_pfd_transaction_list(scs_as_id, bearer_token, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        } else if (app_id.empty()) {
          if (req.method == "PUT") {
            handle_pfd_transaction_put(scs_as_id, trans_id, req.body, bearer_token, res);
          } else if (req.method == "DELETE") {
            handle_pfd_transaction_delete(scs_as_id, trans_id, bearer_token, res);
          } else if (req.method == "GET") {
            // Return transaction body via pfd_transaction_list (legacy app-level)
            m_nef_app->set_request_bearer_token(bearer_token);
            nlohmann::json resp_body; int http_code = 0;
            m_nef_app->handle_pfd_transaction_list(scs_as_id, resp_body, http_code, 2);
            m_nef_app->clear_request_bearer_token();
            res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        } else {
          if (req.method == "GET") {
            handle_pfd_app_get(scs_as_id, trans_id, app_id, bearer_token, res);
          } else if (req.method == "PUT") {
            handle_pfd_app_put(scs_as_id, trans_id, app_id, req.body, bearer_token, res);
          } else if (req.method == "PATCH") {
            handle_pfd_app_patch(scs_as_id, trans_id, app_id, req.body, bearer_token, res);
          } else if (req.method == "DELETE") {
            handle_pfd_app_delete(scs_as_id, trans_id, app_id, bearer_token, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        }
      });

  // Nnef_PFDmanagement /nnef-pfdmanagement/v1/transactions[/{transId}[/applications/{appId}]]
  server_.handle(
      "/nnef-pfdmanagement/",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        const std::string pfx = "/nnef-pfdmanagement/v1/";
        const auto pfx_pos = req.path.find(pfx);
        if (pfx_pos == std::string::npos) {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
        }
        auto rest = req.path.substr(pfx_pos + pfx.size());
        auto s1 = rest.find('/');
        const auto resource = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
        const auto after_resource = (s1 != std::string::npos) ? rest.substr(s1 + 1) : "";
        if (resource != "transactions") {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
        }
        if (after_resource.empty()) {
          if (req.method == "GET") {
            handle_nnef_pfd_list_transactions(bearer_token, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
          return;
        }
        auto s2 = after_resource.find('/');
        const auto trans_id = (s2 != std::string::npos) ? after_resource.substr(0, s2) : after_resource;
        const auto after_trans_id = (s2 != std::string::npos) ? after_resource.substr(s2 + 1) : "";
        if (trans_id.empty()) {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
        }
        if (after_trans_id.empty()) {
          if (req.method == "GET") {
            handle_nnef_pfd_get_transaction(trans_id, bearer_token, res);
          } else if (req.method == "PUT") {
            handle_nnef_pfd_put_transaction(trans_id, req.body, bearer_token, res);
          } else if (req.method == "DELETE") {
            handle_nnef_pfd_delete_transaction(trans_id, bearer_token, res);
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
        if (req.method == "GET") {
          handle_nnef_pfd_get_app(trans_id, app_id, bearer_token, res);
        } else if (req.method == "PUT") {
          handle_nnef_pfd_put_app(trans_id, app_id, req.body, bearer_token, res);
        } else if (req.method == "DELETE") {
          handle_nnef_pfd_delete_app(trans_id, app_id, bearer_token, res);
        } else {
          end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
        }
      });

  // Nnef_PFDmanagement — /nnef-pfdmanagement/v1/applications[/partial-pull]
  server_.handle(
      "/nnef-pfdmanagement/v1/applications",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        if (req.path.size() >= 13 &&
            req.path.substr(req.path.size() - 13) == "/partial-pull") {
          if (req.method == "POST") {
            handle_nnef_pfd_partial_pull(req.body, bearer_token, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported");
          }
          return;
        }
        if (req.method == "GET") {
          std::vector<std::string> ids;
          std::string q = req.raw_query;
          while (!q.empty()) {
            auto amp = q.find('&');
            auto token = (amp != std::string::npos) ? q.substr(0, amp) : q;
            q = (amp != std::string::npos) ? q.substr(amp + 1) : "";
            const std::string key = "app-ids=";
            if (token.substr(0, key.size()) == key)
              ids.push_back(token.substr(key.size()));
          }
          handle_nnef_pfd_get_applications(ids, bearer_token, res);
        } else {
          end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported");
        }
      });

  // Nnef_PFDmanagement — /nnef-pfdmanagement/v1/subscriptions[/{subId}]
  server_.handle(
      "/nnef-pfdmanagement/v1/subscriptions",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        const std::string pfx = "/nnef-pfdmanagement/v1/subscriptions";
        auto rest = req.path.substr(pfx.size());
        if (rest.empty() || rest == "/") {
          if (req.method == "POST") {
            handle_nnef_pfd_subscription_create(req.body, bearer_token, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported");
          }
          return;
        }
        const auto sub_id = (rest[0] == '/') ? rest.substr(1) : rest;
        if (sub_id.empty()) {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found"); return;
        }
        if (req.method == "GET") {
          handle_nnef_pfd_subscription_get(sub_id, bearer_token, res);
        } else if (req.method == "PUT") {
          handle_nnef_pfd_subscription_put(sub_id, req.body, bearer_token, res);
        } else if (req.method == "DELETE") {
          handle_nnef_pfd_subscription_delete(sub_id, bearer_token, res);
        } else {
          end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported");
        }
      });

  // BDT  /3gpp-bdt/v1/{scsAsId}/[policies|bdtPolicies][/{polId}]
  server_.handle(
      "/3gpp-bdt/",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        const std::string pfx = "/3gpp-bdt/v1/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
        auto s1 = rest.find('/');
        auto af_id = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
        auto after = (s1 != std::string::npos) ? rest.substr(s1 + 1) : "";
        auto s2 = after.find('/');
        auto policy_path = (s2 != std::string::npos) ? after.substr(0, s2) : after;
        auto pol_id = (s2 != std::string::npos) ? after.substr(s2 + 1) : "";
        bool legacy_path = (policy_path == "policies");
        if (legacy_path) {
          Logger::nef_sbi().warn(
              "HTTP/2: BDT request on deprecated path '/policies' (af_id='{}'); "
              "please migrate to canonical '/bdtPolicies' path (TS 29.122 §5.13)",
              af_id);
        }
        if (req.method == "PATCH" && policy_path == "bdtPolicies" && !pol_id.empty()) {
          handle_bdt_patch(af_id, pol_id, req.body, bearer_token, res);
        } else if (req.method == "POST") {
          handle_bdt_create(af_id, req.body, bearer_token, res, legacy_path);
        } else if (req.method == "GET" && pol_id.empty()) {
          handle_bdt_get(af_id, "", bearer_token, res, legacy_path);
        } else if (req.method == "GET" && !pol_id.empty()) {
          handle_bdt_get(af_id, pol_id, bearer_token, res, legacy_path);
        } else if (req.method == "PUT" && !pol_id.empty()) {
          handle_bdt_update(af_id, pol_id, req.body, bearer_token, res, legacy_path);
        } else if (req.method == "DELETE" && !pol_id.empty()) {
          handle_bdt_delete(af_id, pol_id, bearer_token, res, legacy_path);
        } else {
          end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
        }
      });

  // QoS  /3gpp-as-session-with-qos/v1/{afId}/subscriptions[/{subId}]
  server_.handle(
      "/3gpp-as-session-with-qos/",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        const std::string pfx = "/3gpp-as-session-with-qos/v1/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
        auto s1 = rest.find('/');
        auto af_id = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
        auto after = (s1 != std::string::npos) ? rest.substr(s1 + 1) : "";
        auto s2 = after.find('/');
        auto sub_path = (s2 != std::string::npos) ? after.substr(0, s2) : after;
        auto sub_id = (s2 != std::string::npos) ? after.substr(s2 + 1) : "";
        if (sub_path == "subscriptions") {
          if (req.method == "POST" && sub_id.empty()) {
            handle_qos_create(af_id, req.body, bearer_token, res);
          } else if (req.method == "GET" && sub_id.empty()) {
            handle_qos_get(af_id, "", bearer_token, res);
          } else if (req.method == "GET" && !sub_id.empty()) {
            handle_qos_get(af_id, sub_id, bearer_token, res);
          } else if (req.method == "PUT" && !sub_id.empty()) {
            handle_qos_update(af_id, sub_id, req.body, bearer_token, res);
          } else if (req.method == "PATCH" && !sub_id.empty()) {
            handle_qos_patch(af_id, sub_id, req.body, bearer_token, res);
          } else if (req.method == "DELETE" && !sub_id.empty()) {
            handle_qos_delete(af_id, sub_id, bearer_token, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        } else {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found");
        }
      });

  // Analytics  /3gpp-analyticsexposure/v1/{afId}/[fetch|subscriptions[/{subId}]]
  server_.handle(
      "/3gpp-analyticsexposure/",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        const std::string pfx = "/3gpp-analyticsexposure/v1/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
        auto s1 = rest.find('/');
        auto af_id = (s1 != std::string::npos) ? rest.substr(0, s1) : rest;
        auto after = (s1 != std::string::npos) ? rest.substr(s1 + 1) : "";
        auto s2 = after.find('/');
        auto sub_path = (s2 != std::string::npos) ? after.substr(0, s2) : after;
        auto sub_id = (s2 != std::string::npos) ? after.substr(s2 + 1) : "";
        if (after == "fetch" && req.method == "POST") {
          handle_analytics_fetch(af_id, req.body, bearer_token, res);
        } else if (sub_path == "subscriptions") {
          if (req.method == "POST" && sub_id.empty()) {
            handle_analytics_create(af_id, req.body, bearer_token, res);
          } else if (req.method == "GET" && sub_id.empty()) {
            handle_analytics_get(af_id, "", bearer_token, res);
          } else if (req.method == "GET" && !sub_id.empty()) {
            handle_analytics_get(af_id, sub_id, bearer_token, res);
          } else if (req.method == "PUT" && !sub_id.empty()) {
            handle_analytics_update(af_id, sub_id, req.body, bearer_token, res);
          } else if (req.method == "DELETE" && !sub_id.empty()) {
            handle_analytics_delete(af_id, sub_id, bearer_token, res);
          } else {
            end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
          }
        } else {
          end_http2_error(res, 404, "Not Found", "Requested resource was not found");
        }
      });

  // Inbound NF notification receive endpoint
  // AMF/SMF/PCF POST to: /nef-notify/v1/notify/{nf_sub_id}
  server_.handle(
      "/nef-notify/",
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (m_draining.load(std::memory_order_relaxed)) {
          end_http2_error(res, 503, "Service Unavailable", "Server is shutting down"); return;
        }
        const std::string& rate_key = !bearer_token.empty() ? bearer_token : req.peer_address;
        if (!rate_key.empty() && !nef_rate_limiter::instance().allow(rate_key)) {
          end_http2_error(res, 429, "Too Many Requests", "Rate limit exceeded"); return;
        }
        if (req.method == "POST") {
          const std::string prefix = "/nef-notify/v1/notify/";
          std::string nf_sub_id;
          auto pos = req.path.find(prefix);
          if (pos != std::string::npos) {
            nf_sub_id = req.path.substr(pos + prefix.size());
            while (!nf_sub_id.empty() && nf_sub_id.back() == '/')
              nf_sub_id.pop_back();
          }
          Logger::nef_sbi().debug(
              "Received NF notification for sub-id: {}", nf_sub_id);
          handle_nf_notify(nf_sub_id, req.body, bearer_token, res);
        } else {
          end_http2_error(res, 405, "Method Not Allowed", "HTTP method is not supported for this resource");
        }
      });

  // Health check endpoint
  server_.handle(
      "/healthz",
      [this](const http2_request& req, http2_response& res) {
        if (req.method != "GET") {
          end_http2_error(res, 405, "Method Not Allowed",
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
        res.send(http_code, {{"content-type", "application/json"}}, body);
      });

  server_.start();
}


void nef_http2_server::stop() {
  server_.stop();
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
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  std::string    sub_id;
  nlohmann::json resp_body;
  int            http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_monitoring_event_subscription_create(
      scs_as_id, json_body, sub_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_monitoring_event_unsubscribe(
    const std::string& scs_as_id,
    const std::string& sub_id,
    const std::string& bearer_token,
    http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_monitoring_event_subscription_delete(
      scs_as_id, sub_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_monitoring_event_get(
    const std::string& scs_as_id,
    const std::string& sub_id,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json resp_body;
  int            http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_monitoring_event_subscription_get(
      scs_as_id, sub_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_create(
    const std::string& af_id,
    const std::string& body,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  std::string ti_id;
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_traffic_influence_create(
      af_id, json_body, ti_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_update(
    const std::string& af_id,
    const std::string& ti_id,
    const std::string& body,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_traffic_influence_update(
      af_id, ti_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_delete(
    const std::string& af_id,
    const std::string& ti_id,
    const std::string& bearer_token,
    http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_traffic_influence_delete(af_id, ti_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_create(
    const std::string& app_id,
    const std::string& body,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_create(app_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_delete(
    const std::string& app_id,
    const std::string& bearer_token,
    http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_delete(app_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nf_notify(
    const std::string& nf_sub_id,
    const std::string& body,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  if (!body.empty()) {
    try {
      json_body = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
      Logger::nef_sbi().warn(
          "Failed to parse NF notification body: {}", e.what());
      end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
    }
  }
  // Delegate to nef_app which looks up nf_sub_id → af_sub_id and forwards
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nf_notification(nf_sub_id, json_body);
  m_nef_app->clear_request_bearer_token();
  // Acknowledge to the NF
  res.send(204, {}, "");
}

//------------------------------------------------------------------------------
// BDT handlers
void nef_http2_server::handle_bdt_create(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res,
    bool deprecated) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  std::string bdt_id;
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_bdt_policy_create(af_id, json_body, bdt_id, resp_body,
                                       http_code, 2);
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> h;
  h["content-type"] = "application/json";
  if (deprecated) h["x-deprecated"] = "true";
  res.send(http_code, h, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& body,
    const std::string& bearer_token, http2_response& res,
    bool deprecated) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_bdt_policy_update(af_id, bdt_id, json_body, resp_body,
                                       http_code, 2);
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> h;
  h["content-type"] = "application/json";
  if (deprecated) h["x-deprecated"] = "true";
  res.send(http_code, h, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_delete(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& bearer_token, http2_response& res,
    bool deprecated) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_bdt_policy_delete(af_id, bdt_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> h;
  if (deprecated) h["x-deprecated"] = "true";
  res.send(http_code, h, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_get(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& bearer_token, http2_response& res,
    bool deprecated) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  if (bdt_id.empty()) {
    m_nef_app->handle_bdt_policy_list(af_id, resp_body, http_code, 2);
  } else {
    m_nef_app->handle_bdt_policy_get(af_id, bdt_id, resp_body, http_code, 2);
  }
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> h;
  h["content-type"] = "application/json";
  if (deprecated) h["x-deprecated"] = "true";
  res.send(http_code, h, resp_body.dump());
}

// QoS handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_create(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  std::string sub_id;
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_qos_subscription_create(af_id, json_body, sub_id,
                                             resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_delete(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_qos_subscription_delete(af_id, sub_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_get(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  if (sub_id.empty()) {
    m_nef_app->handle_qos_subscription_list(af_id, resp_body, http_code, 2);
  } else {
    m_nef_app->handle_qos_subscription_get(af_id, sub_id, resp_body,
                                            http_code, 2);
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

// Analytics handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_create(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  std::string sub_id;
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_analytics_subscription_create(af_id, json_body, sub_id,
                                                   resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_delete(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_analytics_subscription_delete(af_id, sub_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_get(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  if (sub_id.empty()) {
    m_nef_app->handle_analytics_subscription_list(af_id, resp_body,
                                                   http_code, 2);
  } else {
    m_nef_app->handle_analytics_subscription_get(af_id, sub_id, resp_body,
                                                  http_code, 2);
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

// TI PATCH
//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const std::string& patch_body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_patch = {};
  try { json_patch = nlohmann::json::parse(patch_body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_traffic_influence_patch(af_id, ti_id, json_patch, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

// QoS PATCH
//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_patch(
    const std::string& af_id, const std::string& sub_id,
    const std::string& patch_body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_patch = {};
  try { json_patch = nlohmann::json::parse(patch_body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_qos_subscription_patch(af_id, sub_id, json_patch, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

// PFD transaction-level and app-level handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_list(
    const std::string& scs_as_id,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_transaction_list(scs_as_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_transaction_put(scs_as_id, trans_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_transaction_delete(scs_as_id, trans_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_app_get(scs_as_id, trans_id, app_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_app_put(scs_as_id, trans_id, app_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& patch_body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_patch = {};
  try { json_patch = nlohmann::json::parse(patch_body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_app_patch(scs_as_id, trans_id, app_id, json_patch, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_app_delete(scs_as_id, trans_id, app_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_list_transactions(
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_list_transactions(resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_put_transaction(
    const std::string& trans_id,
    const std::string& body,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_put_transaction(
      trans_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_transaction(
    const std::string& trans_id,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_get_transaction(
      trans_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_delete_transaction(
    const std::string& trans_id,
    const std::string& bearer_token,
    http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_delete_transaction(trans_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_app(
    const std::string& trans_id,
    const std::string& app_id,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_get_app(
      trans_id, app_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_put_app(
    const std::string& trans_id,
    const std::string& app_id,
    const std::string& body,
    const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_put_app(
      trans_id, app_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_delete_app(
    const std::string& trans_id,
    const std::string& app_id,
    const std::string& bearer_token,
    http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_delete_app(trans_id, app_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

// Analytics UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_update(
    const std::string& af_id, const std::string& sub_id,
    const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_analytics_subscription_update(af_id, sub_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

// Nnef_PFDmanagement extra endpoints
//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_get_applications(app_ids_filter, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_partial_pull(
    const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) { json_body = {}; }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_partial_pull(json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_create(
    const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  std::string sub_id;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_subscription_create(
      json_body, sub_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> h;
  h["content-type"] = "application/json";
  if (http_code == 201 && !sub_id.empty()) {
    const std::string loc =
        m_address + "/nnef-pfdmanagement/v1/subscriptions/" + sub_id;
    h["location"] = loc;
  }
  res.send(http_code, h, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_get(
    const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_subscription_get(sub_id, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_put(
    const std::string& sub_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try { json_body = nlohmann::json::parse(body); } catch (...) {
    end_http2_error(res, 400, "Bad Request", "Missing or invalid request payload"); return;
  }
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_subscription_put(sub_id, json_body, resp_body, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_delete(
    const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = 0;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_subscription_delete(sub_id, http_code, 2);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}
