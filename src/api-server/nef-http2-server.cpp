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
void nef_http2_server::handle_ti_get(
    const std::string& af_id, const std::string& ti_id,
    const std::string& bearer_token, http2_response& res) {
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->handle_traffic_influence_get(af_id, ti_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
// TI LIST
void nef_http2_server::handle_ti_list(
    const std::string& af_id, const std::string& bearer_token,
    http2_response& res) {
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->handle_traffic_influence_list(af_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
// Monitoring Event UPDATE (PUT)
void nef_http2_server::handle_monitoring_event_update(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->handle_monitoring_event_subscription_update(
      scs_as_id, sub_id, json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_update(
    const std::string& af_id, const std::string& sub_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_body = nlohmann::json::parse(body);
    m_nef_app->handle_qos_subscription_update(
        af_id, sub_id, json_body, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
// BDT PATCH
void nef_http2_server::handle_bdt_patch(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& patch_body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_patch = {};
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_patch = nlohmann::json::parse(patch_body);
    m_nef_app->handle_bdt_policy_patch(
        af_id, bdt_id, json_patch, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
// Analytics /fetch
void nef_http2_server::handle_analytics_fetch(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->handle_analytics_fetch(af_id, json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_subscribe(
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_body = nlohmann::json::parse(body);
    m_nef_app->handle_nnef_event_exposure_subscribe(
        json_body, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> headers;
  headers["content-type"] = "application/json";
  if (http_code == http_status_code::CREATED && resp_body.contains("self") &&
      resp_body["self"].is_string()) {
    headers["location"] = resp_body["self"].get<std::string>();
  }
  res.send(http_code, headers, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_unsubscribe(
    const std::string& subscription_id, const std::string& bearer_token,
    http2_response& res) {
  m_nef_app->set_request_bearer_token(bearer_token);
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->handle_nnef_event_exposure_unsubscribe(subscription_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {});
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_get(
    const std::string& subscription_id, const std::string& bearer_token,
    http2_response& res) {
  m_nef_app->set_request_bearer_token(bearer_token);
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->handle_nnef_event_exposure_get(
      subscription_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_event_exposure_update(
    const std::string& subscription_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_body = nlohmann::json::parse(body);
    m_nef_app->handle_nnef_event_exposure_update(
        subscription_id, json_body, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::start() {
  Logger::nef_sbi().info(
      "NEF HTTP/2 server listening on {}:{}", m_address, m_port);

  const std::string api_version =
      nef_config_inst->nef()->get_sbi().get_api_version();
  const std::string nnef_event_exposure_base =
      nef_sbi_helper::NnefEventExposureBase + api_version;
  const std::string nef_monitoring_event_base =
      nef_sbi_helper::NefMonitoringEventBase + api_version;
  const std::string nef_traffic_influence_base =
      nef_sbi_helper::NefTrafficInfluenceBase + api_version;
  const std::string nef_pfd_management_base =
      nef_sbi_helper::NefPfdManagementBase + api_version;
  const std::string nnef_pfd_management_base =
      nef_sbi_helper::NnefPfdManagementBase + api_version;
  const std::string nef_bdt_base = nef_sbi_helper::NefBdtBase + api_version;
  const std::string nef_qos_monitoring_base =
      nef_sbi_helper::NefQosMonitoringBase + api_version;
  const std::string nef_analytics_base =
      nef_sbi_helper::NefAnalyticsBase + api_version;
  const std::string nef_notify_base =
      nef_sbi_helper::NefNotifyBase + api_version;

  // Nnef_EventExposure /nnef-eventexposure/v1/subscriptions[/{subscriptionId}]
  server_.handle(
      nnef_event_exposure_base + nef_sbi_helper::NefPathSubscriptions,
      [this, nnef_event_exposure_base](
          const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;

        const std::string pfx = nnef_event_exposure_base + "/";
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
          handle_nnef_event_exposure_unsubscribe(
              subscription_id, bearer_token, res);
        } else {
          end_http2_error(
              res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
              "HTTP method is not supported for this resource");
        }
      });

  // Monitoring Event
  // /3gpp-monitoring-event/v1/{scsAsId}/subscriptions[/{subId}]
  server_.handle(
      nef_monitoring_event_base + "/",
      [this, nef_monitoring_event_base](
          const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
        // /{base}/{ver}/{scsAsId}/subscriptions[/{subId}]
        const std::string pfx = nef_monitoring_event_base + "/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
        // rest = scsAsId[/subscriptions[/subId]]
        std::vector<std::string> path_parts;
        boost::split(path_parts, rest, boost::is_any_of("/"));
        auto scs_as_id = !path_parts.empty() ? path_parts[0] : "";
        auto sub_id    = (path_parts.size() > 2) ? path_parts[2] : "";

        if (req.method == method_e::POST) {
          handle_monitoring_event_subscribe(
              scs_as_id, req.body, bearer_token, res);
        } else if (req.method == method_e::DELETE && !sub_id.empty()) {
          handle_monitoring_event_unsubscribe(
              scs_as_id, sub_id, bearer_token, res);
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
      });

  // Traffic Influence
  // /3gpp-traffic-influence/v1/{afId}/subscriptions[/{appSessionId}]
  server_.handle(
      nef_traffic_influence_base + "/",
      [this, nef_traffic_influence_base](
          const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
        const std::string pfx = nef_traffic_influence_base + "/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
        std::vector<std::string> path_parts;
        boost::split(path_parts, rest, boost::is_any_of("/"));
        auto af_id           = !path_parts.empty() ? path_parts[0] : "";
        std::string sub_path = (path_parts.size() > 1) ? path_parts[1] : "";
        std::string app_session_id =
            (path_parts.size() > 2) ? path_parts[2] : "";

        if (sub_path == nef_sbi_helper::NefResourceSubscriptions) {
          if (req.method == method_e::GET && app_session_id.empty()) {
            handle_ti_list(af_id, bearer_token, res);
          } else if (req.method == method_e::GET && !app_session_id.empty()) {
            handle_ti_get(af_id, app_session_id, bearer_token, res);
          } else if (req.method == method_e::POST && app_session_id.empty()) {
            handle_ti_create(af_id, req.body, bearer_token, res);
          } else if (req.method == method_e::PUT && !app_session_id.empty()) {
            handle_ti_update(
                af_id, app_session_id, req.body, bearer_token, res);
          } else if (req.method == method_e::PATCH && !app_session_id.empty()) {
            handle_ti_patch(af_id, app_session_id, req.body, bearer_token, res);
          } else if (
              req.method == method_e::DELETE && !app_session_id.empty()) {
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
      });

  // PFD Management
  // /3gpp-pfd-management/v1/{scsAsId}/transactions[/{transId}[/applications/{appId}]]
  server_.handle(
      nef_pfd_management_base + "/",
      [this, nef_pfd_management_base](
          const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
        const std::string pfx = nef_pfd_management_base + "/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
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
            handle_pfd_transaction_delete(
                scs_as_id, trans_id, bearer_token, res);
          } else if (req.method == method_e::GET) {
            // Return transaction body via pfd_transaction_list (legacy
            // app-level)
            m_nef_app->set_request_bearer_token(bearer_token);
            nlohmann::json resp_body;
            int http_code = http_status_code::NO_RESPONSE;
            m_nef_app->handle_pfd_transaction_list(
                scs_as_id, resp_body, http_code);
            m_nef_app->clear_request_bearer_token();
            res.send(
                http_code, {{"content-type", "application/json"}},
                resp_body.dump());
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
            handle_pfd_app_delete(
                scs_as_id, trans_id, app_id, bearer_token, res);
          } else {
            end_http2_error(
                res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
                "HTTP method is not supported for this resource");
          }
        }
      });

  // Nnef_PFDmanagement
  // /nnef-pfdmanagement/v1/transactions[/{transId}[/applications/{appId}]]
  server_.handle(
      nnef_pfd_management_base +
          nef_sbi_helper::NnefPfdManagementPathTransactions,
      [this, nnef_pfd_management_base](
          const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
        const std::string pfx =
            nnef_pfd_management_base +
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
            handle_nnef_pfd_put_transaction(
                trans_id, req.body, bearer_token, res);
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
          handle_nnef_pfd_put_app(
              trans_id, app_id, req.body, bearer_token, res);
        } else if (req.method == method_e::DELETE) {
          handle_nnef_pfd_delete_app(trans_id, app_id, bearer_token, res);
        } else {
          end_http2_error(
              res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
              "HTTP method is not supported for this resource");
        }
      });

  // Nnef_PFDmanagement — /nnef-pfdmanagement/v1/applications[/partial-pull]
  server_.handle(
      nnef_pfd_management_base +
          nef_sbi_helper::NnefPfdManagementPathApplications,
      [this](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
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
      });

  // Nnef_PFDmanagement — /nnef-pfdmanagement/v1/subscriptions[/{subId}]
  server_.handle(
      nnef_pfd_management_base +
          nef_sbi_helper::NnefPfdManagementPathSubscriptions,
      [this, nnef_pfd_management_base](
          const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
        const std::string pfx =
            nnef_pfd_management_base +
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
      });

  // BDT  /3gpp-bdt/v1/{scsAsId}/[policies|bdtPolicies][/{polId}]
  server_.handle(
      nef_bdt_base + "/",
      [this, nef_bdt_base](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
        const std::string pfx = nef_bdt_base + "/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
        std::vector<std::string> path_parts;
        boost::split(path_parts, rest, boost::is_any_of("/"));
        auto af_id       = !path_parts.empty() ? path_parts[0] : "";
        auto policy_path = (path_parts.size() > 1) ? path_parts[1] : "";
        auto pol_id      = (path_parts.size() > 2) ? path_parts[2] : "";
        bool legacy_path = (policy_path == nef_sbi_helper::NefResourcePolicies);
        if (legacy_path) {
          Logger::nef_sbi().warn(
              "HTTP/2: BDT request on deprecated path '/policies' "
              "(af_id='{}'); "
              "please migrate to canonical '/bdtPolicies' path (TS 29.122 "
              "§5.13)",
              af_id);
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
          handle_bdt_update(
              af_id, pol_id, req.body, bearer_token, res, legacy_path);
        } else if (req.method == method_e::DELETE && !pol_id.empty()) {
          handle_bdt_delete(af_id, pol_id, bearer_token, res, legacy_path);
        } else {
          end_http2_error(
              res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
              "HTTP method is not supported for this resource");
        }
      });

  // QoS  /3gpp-as-session-with-qos/v1/{afId}/subscriptions[/{subId}]
  server_.handle(
      nef_qos_monitoring_base + "/",
      [this, nef_qos_monitoring_base](
          const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
        const std::string pfx = nef_qos_monitoring_base + "/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
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
      });

  // Analytics /3gpp-analyticsexposure/v1/{afId}/[fetch|subscriptions[/{subId}]]
  server_.handle(
      nef_analytics_base + "/",
      [this, nef_analytics_base](
          const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
        const std::string pfx = nef_analytics_base + "/";
        auto rest = req.path.substr(req.path.find(pfx) + pfx.size());
        std::vector<std::string> path_parts;
        boost::split(path_parts, rest, boost::is_any_of("/"));
        auto af_id    = !path_parts.empty() ? path_parts[0] : "";
        auto sub_path = (path_parts.size() > 1) ? path_parts[1] : "";
        auto sub_id   = (path_parts.size() > 2) ? path_parts[2] : "";
        if (path_parts.size() == 2 &&
            sub_path == nef_sbi_helper::NefResourceFetch &&
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
      });

  // Inbound NF notification receive endpoint
  // AMF/SMF/PCF POST to: /nef-notify/v1/notify/{nf_sub_id}
  server_.handle(
      nef_notify_base + nef_sbi_helper::NefNotifyPathNotify,
      [this, nef_notify_base](const http2_request& req, http2_response& res) {
        const std::string bearer_token = extract_bearer(req);
        if (end_http2_if_draining(m_draining, res)) return;
        if (end_http2_if_rate_limited(req, bearer_token, res)) return;
        if (req.method == method_e::POST) {
          const std::string prefix =
              nef_notify_base + nef_sbi_helper::NefNotifyPathNotify + "/";
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
          end_http2_error(
              res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
              "HTTP method is not supported for this resource");
        }
      });

  // Health check endpoint
  server_.handle(
      nef_sbi_helper::NefHealthPath,
      [this](const http2_request& req, http2_response& res) {
        if (req.method != method_e::GET) {
          end_http2_error(
              res, http_status_code::METHOD_NOT_ALLOWED, "Method Not Allowed",
              "HTTP method is not supported for this resource");
          return;
        }
        const bool draining = m_draining.load(std::memory_order_relaxed);
        const int uptime    = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m_start_time)
                .count());
        const std::string instance_id =
            m_nef_app ? m_nef_app->get_nef_instance_id() : "";
        int http_code          = http_status_code::NO_RESPONSE;
        const std::string body = oai::nef::app::nef_health_check::make_response(
            draining, instance_id, uptime, http_code);
        res.send(http_code, {{"content-type", "application/json"}}, body);
      });

  // Start the server (blocks until stop() is called)
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
    const std::string& scs_as_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  std::string sub_id;
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_monitoring_event_subscription_create(
      scs_as_id, json_body, sub_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_monitoring_event_unsubscribe(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_monitoring_event_subscription_delete(
      scs_as_id, sub_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_monitoring_event_get(
    const std::string& scs_as_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_monitoring_event_subscription_get(
      scs_as_id, sub_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_create(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  std::string ti_id;
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_body = nlohmann::json::parse(body);
    m_nef_app->handle_traffic_influence_create(
        af_id, json_body, ti_id, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_update(
    const std::string& af_id, const std::string& ti_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_body = nlohmann::json::parse(body);
    m_nef_app->handle_traffic_influence_update(
        af_id, ti_id, json_body, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_delete(
    const std::string& af_id, const std::string& ti_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_traffic_influence_delete(af_id, ti_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_create(
    const std::string& app_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_create(app_id, json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_delete(
    const std::string& app_id, const std::string& bearer_token,
    http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_delete(app_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nf_notify(
    const std::string& nf_sub_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  if (!body.empty()) {
    try {
      json_body = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
      Logger::nef_sbi().warn(
          "Failed to parse NF notification body: {}", e.what());
      end_http2_error(
          res, http_status_code::BAD_REQUEST, "Bad Request",
          "Missing or invalid request payload");
      return;
    }
  }
  // Delegate to nef_app which looks up nf_sub_id → af_sub_id and forwards
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nf_notification(nf_sub_id, json_body);
  m_nef_app->clear_request_bearer_token();
  // Acknowledge to the NF
  res.send(http_status_code::NO_CONTENT, {}, "");
}

//------------------------------------------------------------------------------
// BDT handlers
void nef_http2_server::handle_bdt_create(
    const std::string& af_id, const std::string& body,
    const std::string& bearer_token, http2_response& res, bool deprecated) {
  nlohmann::json json_body = {};
  std::string bdt_id;
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_body = nlohmann::json::parse(body);
    m_nef_app->handle_bdt_policy_create(
        af_id, json_body, bdt_id, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> h;
  h["content-type"] = "application/json";
  if (deprecated) h["x-deprecated"] = "true";
  res.send(http_code, h, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_update(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res, bool deprecated) {
  nlohmann::json json_body = {};
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_body = nlohmann::json::parse(body);
    m_nef_app->handle_bdt_policy_update(
        af_id, bdt_id, json_body, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> h;
  h["content-type"] = "application/json";
  if (deprecated) h["x-deprecated"] = "true";
  res.send(http_code, h, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_delete(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& bearer_token, http2_response& res, bool deprecated) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_bdt_policy_delete(af_id, bdt_id, http_code);
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> h;
  if (deprecated) h["x-deprecated"] = "true";
  res.send(http_code, h, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_bdt_get(
    const std::string& af_id, const std::string& bdt_id,
    const std::string& bearer_token, http2_response& res, bool deprecated) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  if (bdt_id.empty()) {
    m_nef_app->handle_bdt_policy_list(af_id, resp_body, http_code);
  } else {
    m_nef_app->handle_bdt_policy_get(af_id, bdt_id, resp_body, http_code);
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
  std::string sub_id;
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_body = nlohmann::json::parse(body);
    m_nef_app->handle_qos_subscription_create(
        af_id, json_body, sub_id, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_delete(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_qos_subscription_delete(af_id, sub_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_get(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  if (sub_id.empty()) {
    m_nef_app->handle_qos_subscription_list(af_id, resp_body, http_code);
  } else {
    m_nef_app->handle_qos_subscription_get(af_id, sub_id, resp_body, http_code);
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
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  std::string sub_id;
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_analytics_subscription_create(
      af_id, json_body, sub_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_delete(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_analytics_subscription_delete(af_id, sub_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_get(
    const std::string& af_id, const std::string& sub_id,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  if (sub_id.empty()) {
    m_nef_app->handle_analytics_subscription_list(af_id, resp_body, http_code);
  } else {
    m_nef_app->handle_analytics_subscription_get(
        af_id, sub_id, resp_body, http_code);
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

// TI PATCH
//------------------------------------------------------------------------------
void nef_http2_server::handle_ti_patch(
    const std::string& af_id, const std::string& ti_id,
    const std::string& patch_body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_patch = {};
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_patch = nlohmann::json::parse(patch_body);
    m_nef_app->handle_traffic_influence_patch(
        af_id, ti_id, json_patch, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

// QoS PATCH
//------------------------------------------------------------------------------
void nef_http2_server::handle_qos_patch(
    const std::string& af_id, const std::string& sub_id,
    const std::string& patch_body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_patch = {};
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  try {
    json_patch = nlohmann::json::parse(patch_body);
    m_nef_app->handle_qos_subscription_patch(
        af_id, sub_id, json_patch, resp_body, http_code);
  } catch (const nlohmann::json::exception& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request", e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    m_nef_app->clear_request_bearer_token();
    end_http2_error(
        res, http_status_code::UNPROCESSABLE_ENTITY, "Unprocessable Entity",
        e.what());
    return;
  }
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

// PFD transaction-level and app-level handlers
//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_list(
    const std::string& scs_as_id, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_transaction_list(scs_as_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_transaction_put(
      scs_as_id, trans_id, json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_transaction_delete(scs_as_id, trans_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_app_get(
      scs_as_id, trans_id, app_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_app_put(
      scs_as_id, trans_id, app_id, json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& patch_body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_patch = {};
  try {
    json_patch = nlohmann::json::parse(patch_body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_app_patch(
      scs_as_id, trans_id, app_id, json_patch, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& bearer_token,
    http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_pfd_app_delete(scs_as_id, trans_id, app_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_list_transactions(
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_list_transactions(resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_put_transaction(
    const std::string& trans_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_put_transaction(
      trans_id, json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_transaction(
    const std::string& trans_id, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_get_transaction(trans_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_delete_transaction(
    const std::string& trans_id, const std::string& bearer_token,
    http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_delete_transaction(trans_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_app(
    const std::string& trans_id, const std::string& app_id,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_get_app(trans_id, app_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_put_app(
    const std::string& trans_id, const std::string& app_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_put_app(
      trans_id, app_id, json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_delete_app(
    const std::string& trans_id, const std::string& app_id,
    const std::string& bearer_token, http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_delete_app(trans_id, app_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}

// Analytics UPDATE (PUT)
//------------------------------------------------------------------------------
void nef_http2_server::handle_analytics_update(
    const std::string& af_id, const std::string& sub_id,
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_analytics_subscription_update(
      af_id, sub_id, json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

// Nnef_PFDmanagement extra endpoints
//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_get_applications(
      app_ids_filter, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_partial_pull(
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    json_body = {};
  }
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_partial_pull(json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_create(
    const std::string& body, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  nlohmann::json resp_body;
  std::string sub_id;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_subscription_create(
      json_body, sub_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  std::map<std::string, std::string> h;
  h["content-type"] = "application/json";
  if (http_code == http_status_code::CREATED && !sub_id.empty()) {
    const std::string loc =
        m_address + nef_sbi_helper::NnefPfdManagementBase +
        nef_config_inst->nef()->get_sbi().get_api_version() +
        nef_sbi_helper::NnefPfdManagementPathSubscriptions + "/" + sub_id;
    h["location"] = loc;
  }
  res.send(http_code, h, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_get(
    const std::string& sub_id, const std::string& bearer_token,
    http2_response& res) {
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_subscription_get(sub_id, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_put(
    const std::string& sub_id, const std::string& body,
    const std::string& bearer_token, http2_response& res) {
  nlohmann::json json_body = {};
  try {
    json_body = nlohmann::json::parse(body);
  } catch (...) {
    end_http2_error(
        res, http_status_code::BAD_REQUEST, "Bad Request",
        "Missing or invalid request payload");
    return;
  }
  nlohmann::json resp_body;
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_subscription_put(
      sub_id, json_body, resp_body, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {{"content-type", "application/json"}}, resp_body.dump());
}

//------------------------------------------------------------------------------
void nef_http2_server::handle_nnef_pfd_subscription_delete(
    const std::string& sub_id, const std::string& bearer_token,
    http2_response& res) {
  int http_code = http_status_code::NO_RESPONSE;
  m_nef_app->set_request_bearer_token(bearer_token);
  m_nef_app->handle_nnef_pfd_subscription_delete(sub_id, http_code);
  m_nef_app->clear_request_bearer_token();
  res.send(http_code, {}, "");
}
