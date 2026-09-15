/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_app.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/bind/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <cctype>
#include <chrono>
#include <ctime>
#include <sstream>

#include "3gpp_29.500.h"
#include "logger.hpp"
#include "nef_audit_log.hpp"
#include "nef_callback_uri_validator.hpp"
#include "nef_input_validation.hpp"
#include "nef_pfd_atomicity.hpp"
#include "nef_pfd_management_quality.hpp"
#include "nef_client.hpp"
#include "nef_config.hpp"
#include "nef_jwt.hpp"
#include "nef_notification_mapper.hpp"
#include "nef_async_parse_helpers.hpp"
#include "nef_sbi_helper.hpp"
#include "nef_sbi_response_policy.hpp"

#include "AmfCreatedEventSubscription.h"
#include "AsSessionWithQoSSubscription.h"
#include "UserPlaneEvent.h"
#include "BdtPolicy.h"
#include "Helpers.h"
#include "NefEvent_anyOf.h"
#include "NefEventExposureSubsc.h"
#include "TrafficInfluData.h"
#include "TrafficInfluDataPatch.h"

#include <algorithm>
#include "nef_app_internal.hpp"

using namespace oai::nef::app;
using namespace boost::placeholders;
using namespace oai::common::sbi;

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

//------------------------------------------------------------------------------
static bool normalize_nnef_pfd_app_data(
    const std::string& app_id, const nlohmann::json& input,
    nlohmann::json& output, std::string& error_detail) {
  if (!input.is_object()) {
    error_detail = "Application PFD must be a JSON object";
    return false;
  }

  output = input;
  if (output.contains("externalAppId")) {
    if (!output["externalAppId"].is_string()) {
      error_detail = "externalAppId must be a string";
      return false;
    }
    if (output["externalAppId"].get<std::string>() != app_id) {
      error_detail = "externalAppId must match the target appId";
      return false;
    }
  } else {
    output["externalAppId"] = app_id;
  }

  if (!output.contains("pfds") || !output["pfds"].is_object()) {
    error_detail = "Missing required field: pfds";
    return false;
  }

  for (auto& [pfd_id, pfd_content] : output["pfds"].items()) {
    if (!pfd_content.is_object()) {
      error_detail = "Each PFD entry must be a JSON object";
      return false;
    }

    if (pfd_content.contains("pfdId")) {
      if (!pfd_content["pfdId"].is_string()) {
        error_detail = "pfdId must be a string";
        return false;
      }
      if (pfd_content["pfdId"].get<std::string>() != pfd_id) {
        error_detail = "pfdId must match its map key";
        return false;
      }
    } else {
      pfd_content["pfdId"] = pfd_id;
    }

    if (pfd_content.contains("flowDescriptions") &&
        !pfd_content["flowDescriptions"].is_array()) {
      error_detail = "flowDescriptions must be an array when present";
      return false;
    }
    if (pfd_content.contains("urls") && !pfd_content["urls"].is_array()) {
      error_detail = "urls must be an array when present";
      return false;
    }
    if (pfd_content.contains("domainNames") &&
        !pfd_content["domainNames"].is_array()) {
      error_detail = "domainNames must be an array when present";
      return false;
    }
  }

  return true;
}

//------------------------------------------------------------------------------
static bool extract_nnef_pfd_transaction_apps(
    const nlohmann::json& body, nlohmann::json& applications,
    std::string& error_detail) {
  applications = nlohmann::json::object();

  auto add_app = [&](const std::string& app_id,
                     const nlohmann::json& app_body) -> bool {
    if (app_id.empty()) {
      error_detail = "Application identifier is missing";
      return false;
    }
    nlohmann::json normalized;
    if (!normalize_nnef_pfd_app_data(
            app_id, app_body, normalized, error_detail)) {
      return false;
    }
    applications[app_id] = normalized;
    return true;
  };

  auto add_app_array = [&](const nlohmann::json& app_array) -> bool {
    if (!app_array.is_array()) {
      error_detail =
          "applications must be an array when provided as a collection";
      return false;
    }
    for (const auto& app_body : app_array) {
      if (!app_body.is_object() || !app_body.contains("externalAppId") ||
          !app_body["externalAppId"].is_string()) {
        error_detail = "Each application entry must contain externalAppId";
        return false;
      }
      if (!add_app(app_body["externalAppId"].get<std::string>(), app_body)) {
        return false;
      }
    }
    return true;
  };

  if (body.is_array()) {
    if (!add_app_array(body)) return false;
  } else if (body.is_object()) {
    if (body.contains("applications")) {
      if (body["applications"].is_array()) {
        if (!add_app_array(body["applications"])) return false;
      } else if (body["applications"].is_object()) {
        for (const auto& [app_id, app_body] : body["applications"].items()) {
          if (!add_app(app_id, app_body)) return false;
        }
      } else {
        error_detail = "applications must be an object or an array";
        return false;
      }
    } else if (body.contains("pfdDatas") && body["pfdDatas"].is_object()) {
      for (const auto& [app_id, app_body] : body["pfdDatas"].items()) {
        if (!add_app(app_id, app_body)) return false;
      }
    } else if (
        body.contains("externalAppId") && body["externalAppId"].is_string()) {
      if (!add_app(body["externalAppId"].get<std::string>(), body))
        return false;
    } else {
      error_detail = "Transaction body must contain applications or pfdDatas";
      return false;
    }
  } else {
    error_detail = "Transaction body must be a JSON object or array";
    return false;
  }

  if (applications.empty()) {
    error_detail = "Transaction must contain at least one application PFD";
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
static nlohmann::json make_nnef_pfd_transaction(
    const std::string& transaction_id, const nlohmann::json& body,
    const nlohmann::json& applications) {
  nlohmann::json transaction =
      body.is_object() ? body : nlohmann::json::object();
  transaction["transactionId"] = transaction_id;
  transaction["applications"]  = applications;
  if (transaction.contains("pfdDatas")) {
    transaction.erase("pfdDatas");
  }
  return transaction;
}

// PFD transaction-level and app-level endpoints (TS 29.122)
//------------------------------------------------------------------------------
void nef_app::handle_pfd_transaction_list(
    const std::string& scs_as_id, nlohmann::json& response_body,
    int& http_code) {
  if (reject_unauthorized_af(
          scs_as_id, NEF_SERVICE_PFD_MANAGEMENT, response_body, http_code)) {
    return;
  }
  std::shared_lock lock(m_pfd_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [tid, app_map] : m_pfd_trans_sessions) {
    auto owner_it = m_pfd_trans2scs_id.find(tid);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id)
      continue;
    nlohmann::json entry;
    entry["transId"]  = tid;
    entry["pfdDatas"] = nlohmann::json::object();
    for (const auto& [app_id, app] : app_map) {
      nlohmann::json app_json;
      to_json(app_json, app);
      entry["pfdDatas"][app_id] = app_json;
    }
    response_body.push_back(entry);
  }
  http_code = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_pfd_app_get(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_af(
          scs_as_id, NEF_SERVICE_PFD_MANAGEMENT, response_body, http_code)) {
    return;
  }
  std::shared_lock lock(m_pfd_mutex);
  auto it = m_pfd_trans_sessions.find(trans_id);
  if (it == m_pfd_trans_sessions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "PFD transaction not found");
    return;
  }
  auto owner_it = m_pfd_trans2scs_id.find(trans_id);
  if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
    http_code     = http_status_code::FORBIDDEN;
    response_body = make_problem_detail(
        http_status_code::FORBIDDEN,
        "AF is not allowed to access this resource");
    return;
  }
  const auto& app_map = it->second;
  auto app_it         = app_map.find(app_id);
  if (app_it == app_map.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND,
        "Application PFD not found in transaction");
    return;
  }
  to_json(response_body, app_it->second);
  response_body["appId"] = app_id;
  http_code              = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_list_transactions(
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_nf(
          NEF_SERVICE_PFD_MANAGEMENT, response_body, http_code)) {
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [transaction_id, transaction] : m_nnef_pfd_transactions) {
    response_body.push_back(transaction);
  }
  http_code = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_get_transaction(
    const std::string& transaction_id, nlohmann::json& response_body,
    int& http_code) {
  if (reject_unauthorized_nf(
          NEF_SERVICE_PFD_MANAGEMENT, response_body, http_code)) {
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto it = m_nnef_pfd_transactions.find(transaction_id);
  if (it == m_nnef_pfd_transactions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "PFD transaction not found");
    return;
  }
  response_body = it->second;
  http_code     = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_get_app(
    const std::string& transaction_id, const std::string& app_id,
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_nf(
          NEF_SERVICE_PFD_MANAGEMENT, response_body, http_code)) {
    return;
  }
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  auto transaction_it = m_nnef_pfd_transactions.find(transaction_id);
  if (transaction_it == m_nnef_pfd_transactions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "PFD transaction not found");
    return;
  }

  const auto& transaction = transaction_it->second;
  if (!transaction.contains("applications") ||
      !transaction["applications"].is_object() ||
      !transaction["applications"].contains(app_id)) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND,
        "Application PFD not found in transaction");
    return;
  }

  response_body = transaction["applications"][app_id];
  http_code     = http_status_code::OK;
}

// Nnef_PFDmanagement — GET /applications
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_get_applications(
    const std::vector<std::string>& app_ids_filter,
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_nf(
          NEF_SERVICE_PFD_MANAGEMENT, response_body, http_code)) {
    return;
  }
  // Returns all PFD apps across all transactions, filtered by app-ids param.
  std::shared_lock lock(m_nnef_pfd_transactions_mutex);
  response_body = nlohmann::json::array();
  for (const auto& [trans_id, transaction] : m_nnef_pfd_transactions) {
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object())
      continue;
    for (const auto& [app_id, app_data] : transaction["applications"].items()) {
      if (!app_ids_filter.empty()) {
        bool found = false;
        for (const auto& f : app_ids_filter) {
          if (f == app_id) {
            found = true;
            break;
          }
        }
        if (!found) continue;
      }
      nlohmann::json entry = app_data;
      entry["transId"]     = trans_id;
      response_body.push_back(entry);
    }
  }
  http_code = http_status_code::OK;
}

// Nnef_PFDmanagement — subscription CRUD
//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_create(
    const nlohmann::json& body, std::string& sub_id,
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_nf(
          NEF_SERVICE_PFD_MANAGEMENT, response_body, http_code)) {
    return;
  }
  // Normalize: wire uses "notifUri", model uses "notifyUri"
  nlohmann::json normalized = body;
  if (normalized.contains("notifUri")) {
    normalized["notifyUri"] = normalized["notifUri"];
    normalized.erase("notifUri");
  }

  oai::_3gpp::model::PfdSubscription sub;
  try {
    from_json(normalized, sub);
    sub.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code = http_status_code::BAD_REQUEST;
    response_body =
        make_problem_detail(http_status_code::BAD_REQUEST, e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code = http_status_code::UNPROCESSABLE_ENTITY;
    response_body =
        make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, e.what());
    return;
  } catch (const std::exception& e) {
    // e.g. std::invalid_argument thrown by a generated enum from_json on an
    // unrecognised value. Treat as a malformed request body (400) rather than
    // letting it escape and abort the process.
    http_code = http_status_code::BAD_REQUEST;
    response_body =
        make_problem_detail(http_status_code::BAD_REQUEST, e.what());
    return;
  }

  if (sub.getNotifyUri().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "notifUri is required");
    return;
  }
  const std::string uri_err = validate_callback_uri(sub.getNotifyUri());
  if (!uri_err.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "notifUri: " + uri_err);
    return;
  }

  generate_af_subscription_id(sub_id);

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_subscriptions_mutex);
    m_nnef_pfd_subscriptions[sub_id] = sub;
  }

  // Build response: serialize typed, denormalize back to wire key
  to_json(response_body, sub);
  if (response_body.contains("notifyUri")) {
    response_body["notifUri"] = response_body["notifyUri"];
    response_body.erase("notifyUri");
  }
  response_body["subId"] = sub_id;
  http_code              = http_status_code::CREATED;
  nef_audit::log("CREATE", "NNEF_PFD_SUB", "", sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_get(
    const std::string& sub_id, nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_nf(
          NEF_SERVICE_PFD_MANAGEMENT, response_body, http_code)) {
    return;
  }
  std::shared_lock lock(m_nnef_pfd_subscriptions_mutex);
  auto it = m_nnef_pfd_subscriptions.find(sub_id);
  if (it == m_nnef_pfd_subscriptions.end()) {
    http_code     = http_status_code::NOT_FOUND;
    response_body = make_problem_detail(
        http_status_code::NOT_FOUND, "PFD subscription not found");
    return;
  }
  // Serialize typed, denormalize wire key
  to_json(response_body, it->second);
  if (response_body.contains("notifyUri")) {
    response_body["notifUri"] = response_body["notifyUri"];
    response_body.erase("notifyUri");
  }
  response_body["subId"] = sub_id;
  http_code              = http_status_code::OK;
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_put(
    const std::string& sub_id, const nlohmann::json& body,
    nlohmann::json& response_body, int& http_code) {
  if (reject_unauthorized_nf(
          NEF_SERVICE_PFD_MANAGEMENT, response_body, http_code)) {
    return;
  }
  // Normalize: wire uses "notifUri", model uses "notifyUri"
  nlohmann::json normalized = body;
  if (normalized.contains("notifUri")) {
    normalized["notifyUri"] = normalized["notifUri"];
    normalized.erase("notifUri");
  }

  oai::_3gpp::model::PfdSubscription sub;
  try {
    from_json(normalized, sub);
    sub.validate();
  } catch (const nlohmann::json::exception& e) {
    http_code = http_status_code::BAD_REQUEST;
    response_body =
        make_problem_detail(http_status_code::BAD_REQUEST, e.what());
    return;
  } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
    http_code = http_status_code::UNPROCESSABLE_ENTITY;
    response_body =
        make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, e.what());
    return;
  } catch (const std::exception& e) {
    // e.g. std::invalid_argument thrown by a generated enum from_json on an
    // unrecognised value. Treat as a malformed request body (400) rather than
    // letting it escape and abort the process.
    http_code = http_status_code::BAD_REQUEST;
    response_body =
        make_problem_detail(http_status_code::BAD_REQUEST, e.what());
    return;
  }

  if (sub.getNotifyUri().empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "notifUri is required");
    return;
  }
  const std::string uri_err = validate_callback_uri(sub.getNotifyUri());
  if (!uri_err.empty()) {
    http_code     = http_status_code::BAD_REQUEST;
    response_body = make_problem_detail(
        http_status_code::BAD_REQUEST, "notifUri: " + uri_err);
    return;
  }

  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_subscriptions_mutex);
    auto it = m_nnef_pfd_subscriptions.find(sub_id);
    if (it == m_nnef_pfd_subscriptions.end()) {
      http_code     = http_status_code::NOT_FOUND;
      response_body = make_problem_detail(
          http_status_code::NOT_FOUND, "PFD subscription not found");
      return;
    }
    it->second = sub;
  }

  // Build response: denormalize wire key
  to_json(response_body, sub);
  if (response_body.contains("notifyUri")) {
    response_body["notifUri"] = response_body["notifyUri"];
    response_body.erase("notifyUri");
  }
  response_body["subId"] = sub_id;
  http_code              = http_status_code::OK;
  nef_audit::log("UPDATE", "NNEF_PFD_SUB", "", sub_id, http_code);
}

//------------------------------------------------------------------------------
void nef_app::handle_nnef_pfd_subscription_delete(
    const std::string& sub_id, int& http_code) {
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    http_code = http_status_code::FORBIDDEN;
    return;
  }
  const std::lock_guard<std::shared_mutex> lock(m_nnef_pfd_subscriptions_mutex);
  auto it = m_nnef_pfd_subscriptions.find(sub_id);
  if (it == m_nnef_pfd_subscriptions.end()) {
    http_code = http_status_code::NOT_FOUND;
    return;
  }
  m_nnef_pfd_subscriptions.erase(it);
  http_code = http_status_code::NO_CONTENT;
  nef_audit::log("DELETE", "NNEF_PFD_SUB", "", sub_id, http_code);
}

// Notify Nnef_PFDmanagement subscribers
//------------------------------------------------------------------------------
void nef_app::notify_nnef_pfd_subscribers(
    const std::string& event_type, const std::string& app_id,
    const nlohmann::json& pfd_data) {
  std::vector<std::pair<std::string, std::string>>
      targets;  // (sub_id, notifUri)
  {
    std::shared_lock lock(m_nnef_pfd_subscriptions_mutex);
    for (const auto& [sid, sub] : m_nnef_pfd_subscriptions) {
      const std::string& notify_uri = sub.getNotifyUri();
      if (notify_uri.empty()) continue;
      // Use typed overload: checks applicationIds filter
      if (!nnef_pfd_subscription_matches(sub, app_id)) continue;
      targets.emplace_back(sid, notify_uri);
    }
  }

  for (const auto& [sid, notif_uri] : targets) {
    nlohmann::json notif;
    notif["eventType"] = event_type;
    notif["appId"]     = app_id;
    if (!pfd_data.is_null()) notif["pfdData"] = pfd_data;

    const std::string notif_str = notif.dump();
    m_notification_pool->enqueue([this, notif_uri, notif_str, sid]() {
      m_nef_client->forward_notification_to_af(notif_uri, notif_str);
    });
  }
}

//------------------------------------------------------------------------------
// pfd_app_put — one southbound call. The not-found, forbidden and parse checks
// and the local store are unchanged; only the UDR PFD PUT becomes an async
// fire.
//
// BEST-EFFORT: cont_pfd_app_put builds the response from the locally-stored
// app JSON whatever UDR says. A UDR failure is warn-only.
void nef_app::pfd_app_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_af(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT, sink)) {
    return;
  }

  bool is_create = false;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "PFD transaction not found")
              .dump());
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN,
              "AF is not allowed to access this resource")
              .dump());
    }
    oai::_3gpp::model::PfdDataForApp new_app;
    try {
      from_json(body, new_app);
      new_app.validate();
    } catch (const nlohmann::json::exception& e) {
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(http_status_code::BAD_REQUEST, e.what()).dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, e.what())
              .dump());
    } catch (const std::exception& e) {
      // e.g. std::invalid_argument thrown by a generated enum from_json on an
      // unrecognised value. Treat as a malformed request body (400) rather
      // than letting it escape and abort the process.
      clear_request_bearer_token();
      return sink(
          http_status_code::BAD_REQUEST,
          make_problem_detail(http_status_code::BAD_REQUEST, e.what()).dump());
    }
    is_create          = (it->second.find(app_id) == it->second.end());
    it->second[app_id] = new_app;
  }

  nlohmann::json new_app_json;
  {
    std::shared_lock rlock(m_pfd_mutex);
    to_json(new_app_json, m_pfd_trans_sessions.at(trans_id).at(app_id));
  }
  clear_request_bearer_token();

  // Fire the UDR PFD PUT.
  m_nef_client->udr_put_pfd_data_async(
      app_id, new_app_json,
      [this, scs_as_id, app_id, new_app_json, is_create,
       sink = std::move(sink)](oai::sba::response r) mutable {
        cont_pfd_app_put(
            scs_as_id, app_id, std::move(new_app_json), is_create, std::move(r),
            std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_pfd_app_put(
    const std::string& scs_as_id, const std::string& app_id,
    nlohmann::json new_app_json, bool is_create, oai::sba::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_app_put app_id=%s status=%d", app_id.c_str(), r.status_code);
  // Best-effort: UDR failure is warn-only; the response is the locally-stored
  // app data regardless of the southbound outcome.
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app PUT failed for app: %s", app_id.c_str());
  }
  nlohmann::json response_body = std::move(new_app_json);
  response_body["appId"]       = app_id;
  const int http_code =
      is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "PFD_APP", scs_as_id, app_id, http_code);
  sink(http_code, response_body.dump());
}

//------------------------------------------------------------------------------
// pfd_create. Authorize, check pfdDatas and validate the path params — all
// unchanged. Only the UDR PFD PUT becomes an async fire.
//
// BEST-EFFORT: cont_pfd_create echoes the request body back as the 201
// whatever UDR says.
void nef_app::pfd_create(
    const std::string& app_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  Logger::nef_app().info("PFD create for app: %s", app_id.c_str());

  if (reject_unauthorized_af(app_id, NEF_SERVICE_PFD_MANAGEMENT, sink)) return;
  if (!body.contains("pfdDatas")) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Missing required field: pfdDatas")
            .dump());
  }
  {
    std::string err;
    if (err.empty()) err = validate_string_param(app_id, "appId", 256);
    if (err.empty()) err = validate_object_field(body, "pfdDatas", false);
    if (!err.empty()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(http_status_code::UNPROCESSABLE_ENTITY, err)
              .dump());
    }
  }
  clear_request_bearer_token();

  // Fire the UDR PFD PUT (best-effort).
  m_nef_client->udr_put_pfd_data_async(
      app_id, body,
      [this, app_id, body,
       sink = std::move(sink)](oai::sba::response r) mutable {
        cont_pfd_create(app_id, std::move(body), std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_pfd_create(
    const std::string& app_id, nlohmann::json body, oai::sba::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_create app_id=%s status=%d", app_id.c_str(), r.status_code);
  // Best-effort: UDR result warn-only.
  if (!sbi_ok(r)) {
    Logger::nef_app().warn("UDR PFD push failed for app: %s", app_id.c_str());
  }
  nef_audit::log("CREATE", "PFD", app_id, app_id, http_status_code::CREATED);
  sink(http_status_code::CREATED, body.dump());
}

//------------------------------------------------------------------------------
// pfd_delete. Authorize — unchanged. Only the UDR PFD delete becomes an async
// fire, and its result is unchecked.
//
// BEST-EFFORT: cont_pfd_delete answers 204 whatever UDR says.
void nef_app::pfd_delete(
    const std::string& app_id, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(app_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  clear_request_bearer_token();

  // Fire the UDR PFD delete
  m_nef_client->udr_delete_pfd_data_async(
      app_id,
      [this, app_id, sink = std::move(sink)](oai::sba::response r) mutable {
        cont_pfd_delete(app_id, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_pfd_delete(
    const std::string& app_id, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_delete app_id=%s status=%d", app_id.c_str(), r.status_code);
  // Best-effort: UDR result ignored.
  if (!sbi_ok(r)) {
    Logger::nef_app().warn("UDR PFD delete failed for app: %s", app_id.c_str());
  }
  nef_audit::log("DELETE", "PFD", app_id, app_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}

//------------------------------------------------------------------------------
// pfd_app_patch. Authorize, check the owner, look up the app, merge-patch,
// typed re-parse and update the local store — all unchanged. Only the UDR PFD
// PUT becomes an async fire.
//
// BEST-EFFORT: cont_pfd_app_patch only warns on a UDR failure, and the 200
// echo body goes back either way.
void nef_app::pfd_app_patch(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const nlohmann::json& patch_body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_af(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT, sink)) {
    return;
  }

  nlohmann::json patched;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND, "PFD transaction not found")
              .dump());
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      clear_request_bearer_token();
      return sink(
          http_status_code::FORBIDDEN,
          make_problem_detail(
              http_status_code::FORBIDDEN,
              "AF is not allowed to access this resource")
              .dump());
    }
    auto app_it = it->second.find(app_id);
    if (app_it == it->second.end()) {
      clear_request_bearer_token();
      return sink(
          http_status_code::NOT_FOUND,
          make_problem_detail(
              http_status_code::NOT_FOUND,
              "Application PFD not found in transaction")
              .dump());
    }
    to_json(patched, app_it->second);
    patched.merge_patch(patch_body);
    oai::_3gpp::model::PfdDataForApp patched_app;
    try {
      from_json(patched, patched_app);
      patched_app.validate();
    } catch (const std::exception& e) {
      clear_request_bearer_token();
      return sink(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY,
              std::string("Patched body invalid: ") + e.what())
              .dump());
    }
    app_it->second = patched_app;
  }
  clear_request_bearer_token();

  // Fire the UDR PFD PUT
  m_nef_client->udr_put_pfd_data_async(
      app_id, patched,
      [this, scs_as_id, app_id, patched,
       sink = std::move(sink)](oai::sba::response r) mutable {
        cont_pfd_app_patch(
            scs_as_id, app_id, std::move(patched), std::move(r),
            std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_pfd_app_patch(
    const std::string& scs_as_id, const std::string& app_id,
    nlohmann::json patched, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_app_patch app_id=%s status=%d", app_id.c_str(), r.status_code);
  // Best-effort: UDR result warn-only.
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app PATCH failed for app: %s", app_id.c_str());
  }
  nlohmann::json response_body = std::move(patched);
  response_body["appId"]       = app_id;
  nef_audit::log("PATCH", "PFD_APP", scs_as_id, app_id, http_status_code::OK);
  sink(http_status_code::OK, response_body.dump());
}

//------------------------------------------------------------------------------
// pfd_app_delete. Authorize, check the owner, look up the app and erase it
// locally — all unchanged. Only the UDR PFD delete becomes an async fire, and
// its result is unchecked.
//
// BEST-EFFORT: cont_pfd_app_delete answers 204 whatever UDR says.
void nef_app::pfd_app_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& app_id, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      clear_request_bearer_token();
      return sink(http_status_code::FORBIDDEN, "");
    }
    if (it->second.find(app_id) == it->second.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    it->second.erase(app_id);
  }
  clear_request_bearer_token();

  // Fire the UDR PFD delete
  m_nef_client->udr_delete_pfd_data_async(
      app_id, [this, scs_as_id, app_id,
               sink = std::move(sink)](oai::sba::response r) mutable {
        cont_pfd_app_delete(scs_as_id, app_id, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_pfd_app_delete(
    const std::string& scs_as_id, const std::string& app_id,
    oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_pfd_app_delete app_id=%s status=%d", app_id.c_str(), r.status_code);
  // Best-effort: UDR result ignored
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app delete failed for app: %s", app_id.c_str());
  }
  nef_audit::log(
      "DELETE", "PFD_APP", scs_as_id, app_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}

//------------------------------------------------------------------------------
// nnef_pfd_put_app. Authorize, normalize the app data and update the local
// transaction store — all unchanged. Only the UDR PFD PUT becomes an async
// fire.
//
// BEST-EFFORT: cont_nnef_pfd_put_app sends the 201 (create) or 200 (update)
// echo body and notifies the PFD subscribers whatever UDR says.
void nef_app::nnef_pfd_put_app(
    const std::string& transaction_id, const std::string& app_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_nf(NEF_SERVICE_PFD_MANAGEMENT, sink)) return;
  std::string error_detail;
  nlohmann::json normalized_app;
  if (!normalize_nnef_pfd_app_data(
          app_id, body, normalized_app, error_detail)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(http_status_code::BAD_REQUEST, error_detail)
            .dump());
  }

  bool is_create = false;
  nlohmann::json response_app;
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto& transaction = m_nnef_pfd_transactions[transaction_id];
    if (!transaction.is_object()) {
      transaction = nlohmann::json::object();
    }
    transaction["transactionId"] = transaction_id;
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object()) {
      transaction["applications"] = nlohmann::json::object();
    }
    is_create = !transaction["applications"].contains(app_id);
    transaction["applications"][app_id] = normalized_app;
    if (transaction.contains("pfdDatas")) {
      transaction.erase("pfdDatas");
    }
    response_app = transaction["applications"][app_id];
  }
  clear_request_bearer_token();

  // Fire the UDR PFD PUT
  m_nef_client->udr_put_pfd_data_async(
      app_id, normalized_app,
      [this, app_id, response_app, normalized_app, is_create,
       sink = std::move(sink)](oai::sba::response r) mutable {
        cont_nnef_pfd_put_app(
            app_id, std::move(response_app), std::move(normalized_app),
            is_create, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_nnef_pfd_put_app(
    const std::string& app_id, nlohmann::json response_app,
    nlohmann::json normalized_app, bool is_create, oai::sba::response r,
    response_sink sink) {
  Logger::nef_app().debug(
      "cont_nnef_pfd_put_app app_id=%s status=%d", app_id.c_str(),
      r.status_code);
  // Best-effort: UDR result warn-only
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app PUT failed for Nnef_PFDmanagement app: %s",
        app_id.c_str());
  }
  const int http_code =
      is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      is_create ? "CREATE" : "UPDATE", "NNEF_PFD_APP", "", app_id, http_code);
  // Notify SBI PFD subscribers (non-blocking; enqueues onto
  // m_notification_pool).
  notify_nnef_pfd_subscribers("PFD_CHANGE", app_id, normalized_app);
  sink(http_code, response_app.dump());
}

//------------------------------------------------------------------------------
// nnef_pfd_delete_app. Authorize, look up the transaction and erase the app
// locally — all unchanged. Only the UDR PFD delete becomes an async fire, and
// a failure is warn-only.
//
// BEST-EFFORT: cont_nnef_pfd_delete_app notifies the PFD subscribers and
// answers 204 whatever UDR says.
void nef_app::nnef_pfd_delete_app(
    const std::string& transaction_id, const std::string& app_id,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto transaction_it = m_nnef_pfd_transactions.find(transaction_id);
    if (transaction_it == m_nnef_pfd_transactions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    auto& transaction = transaction_it->second;
    if (!transaction.contains("applications") ||
        !transaction["applications"].is_object() ||
        !transaction["applications"].contains(app_id)) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    transaction["applications"].erase(app_id);
  }
  clear_request_bearer_token();

  // Fire the UDR PFD delete
  m_nef_client->udr_delete_pfd_data_async(
      app_id,
      [this, app_id, sink = std::move(sink)](oai::sba::response r) mutable {
        cont_nnef_pfd_delete_app(app_id, std::move(r), std::move(sink));
      });
}

//------------------------------------------------------------------------------
void nef_app::cont_nnef_pfd_delete_app(
    const std::string& app_id, oai::sba::response r, response_sink sink) {
  Logger::nef_app().debug(
      "cont_nnef_pfd_delete_app app_id=%s status=%d", app_id.c_str(),
      r.status_code);
  // Best-effort: UDR result warn-only
  if (!sbi_ok(r)) {
    Logger::nef_app().warn(
        "UDR PFD app DELETE failed for Nnef_PFDmanagement app: %s",
        app_id.c_str());
  }
  // Notify SBI PFD subscribers about removal (non-blocking).
  notify_nnef_pfd_subscribers("PFD_REMOVE", app_id, nullptr);
  nef_audit::log(
      "DELETE", "NNEF_PFD_APP", "", app_id, http_status_code::NO_CONTENT);
  sink(http_status_code::NO_CONTENT, "");
}

//------------------------------------------------------------------------------

// ═══════════════════════════════════════════════════════════════════════════
// True-async cursor pattern
//
// Every multi-leg chain below has the same shape:
//
//   1. The dispatcher worker resolves the UDR endpoint via discover_nf.
//   2. It builds a shared_ptr cursor carrying that endpoint, the work set and
//      the response_sink.
//   3. It kicks the cursor.
//   4. Each continuation fires exactly ONE discovery-free *_at_async leg, then
//      advances the cursor, rolls back, or finishes.
//
// Two properties follow, and they are the whole point of the arrangement:
//
//   * No thread is ever parked waiting on a southbound call.
//   * discover_nf never runs on oai-http-io. If it did, the io pool would
//     deadlock against itself.
//
// Continuations are safe to run inline on the dispatcher worker. Two paths do
// exactly that: the client's URI/pool sync fast-path, and a wrapper's own
// discovery-failure callback, which fires inline with status 0.
//
// The synchronous handle_* methods are unchanged.
// ═══════════════════════════════════════════════════════════════════════════

// pfd_transaction_delete — one UDR DELETE per app in the transaction, driven
// by the PfdDeleteChain cursor. BEST-EFFORT: the AF always gets 204.
void nef_app::pfd_transaction_delete(
    const std::string& scs_as_id, const std::string& trans_id,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_af_request(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  std::map<std::string, oai::_3gpp::model::PfdDataForApp> trans_body;
  {
    const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
    auto it = m_pfd_trans_sessions.find(trans_id);
    if (it == m_pfd_trans_sessions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    auto owner_it = m_pfd_trans2scs_id.find(trans_id);
    if (owner_it == m_pfd_trans2scs_id.end() || owner_it->second != scs_as_id) {
      clear_request_bearer_token();
      return sink(http_status_code::FORBIDDEN, "");
    }
    trans_body = it->second;
    m_pfd_trans_sessions.erase(it);
    m_pfd_trans2scs_id.erase(trans_id);
  }

  auto st  = std::make_shared<PfdDeleteChain>();
  st->sink = std::move(sink);
  for (const auto& [app_id, _] : trans_body) st->app_ids.push_back(app_id);

  // Resolve UDR HERE, on the dispatcher worker, and thread it down by value.
  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    // Best-effort: the local state is already erased, and a discovery failure
    // does not change the AF-visible 204. The sync per-app deletes are
    // unchecked too.
    Logger::nef_app().warn(
        "PFD_TX delete: UDR discovery failed for trans_id=%s; skipping UDR "
        "deletes",
        trans_id.c_str());
    nef_audit::log(
        "DELETE", "PFD_TX", scs_as_id, trans_id, http_status_code::NO_CONTENT);
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(http_status_code::NO_CONTENT, "");
  }
  clear_request_bearer_token();

  // Audit the 204 now: it is already decided. Then kick the delete cursor,
  // whose final step sends that 204. The per-app UDR deletes are best-effort
  // and cannot change the result, matching the sync handler's unconditional
  // 204.
  nef_audit::log(
      "DELETE", "PFD_TX", scs_as_id, trans_id, http_status_code::NO_CONTENT);
  pfd_transaction_delete_step(std::move(st));
}

//------------------------------------------------------------------------------
void nef_app::pfd_transaction_delete_step(std::shared_ptr<PfdDeleteChain> st) {
  if (st->idx == st->app_ids.size()) {
    response_sink s = std::move(st->sink);
    return s(http_status_code::NO_CONTENT, "");
  }
  const std::string app_id = st->app_ids[st->idx];
  m_nef_client->udr_delete_pfd_data_at_async(
      st->udr_ep, app_id, [this, st, app_id](oai::sba::response r) mutable {
        // Best-effort and idempotent: failures are ignored, with no
        // compensation. The sync per-app delete leaves its result unchecked
        // too.
        if (!sbi_ok(r)) {
          Logger::nef_app().warn(
              "PFD_TX delete: UDR PFD delete failed for app=%s (http=%d)",
              app_id.c_str(), r.status_code);
        }
        st->idx++;
        pfd_transaction_delete_step(std::move(st));
      });
}

//------------------------------------------------------------------------------
// nnef_pfd_delete_transaction — the Nnef twin of pfd_transaction_delete: one
// UDR DELETE per app over the PfdDeleteChain cursor. BEST-EFFORT 204.
void nef_app::nnef_pfd_delete_transaction(
    const std::string& transaction_id, const std::string& token,
    response_sink sink) {
  set_request_bearer_token(token);
  if (!authorize_nnef_request(NEF_SERVICE_PFD_MANAGEMENT)) {
    clear_request_bearer_token();
    return sink(http_status_code::FORBIDDEN, "");
  }
  nlohmann::json transaction;
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    auto it = m_nnef_pfd_transactions.find(transaction_id);
    if (it == m_nnef_pfd_transactions.end()) {
      clear_request_bearer_token();
      return sink(http_status_code::NOT_FOUND, "");
    }
    transaction = it->second;
    m_nnef_pfd_transactions.erase(it);
  }

  auto st  = std::make_shared<PfdDeleteChain>();
  st->sink = std::move(sink);
  if (transaction.contains("applications") &&
      transaction["applications"].is_object()) {
    for (const auto& [app_id, _] : transaction["applications"].items())
      st->app_ids.push_back(app_id);
  }

  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    Logger::nef_app().warn(
        "NNEF_PFD_TX delete: UDR discovery failed for trans_id=%s; skipping "
        "UDR "
        "deletes",
        transaction_id.c_str());
    nef_audit::log(
        "DELETE", "NNEF_PFD_TX", "", transaction_id,
        http_status_code::NO_CONTENT);
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(http_status_code::NO_CONTENT, "");
  }
  clear_request_bearer_token();

  nef_audit::log(
      "DELETE", "NNEF_PFD_TX", "", transaction_id,
      http_status_code::NO_CONTENT);
  nnef_pfd_delete_transaction_step(std::move(st));
}

//------------------------------------------------------------------------------
void nef_app::nnef_pfd_delete_transaction_step(
    std::shared_ptr<PfdDeleteChain> st) {
  if (st->idx == st->app_ids.size()) {
    response_sink s = std::move(st->sink);
    return s(http_status_code::NO_CONTENT, "");
  }
  const std::string app_id = st->app_ids[st->idx];
  m_nef_client->udr_delete_pfd_data_at_async(
      st->udr_ep, app_id, [this, st, app_id](oai::sba::response r) mutable {
        // Best-effort: a failure here is logged and nothing more.
        if (!sbi_ok(r)) {
          Logger::nef_app().warn(
              "UDR PFD delete failed for Nnef_PFDmanagement app: %s (http=%d)",
              app_id.c_str(), r.status_code);
        }
        st->idx++;
        nnef_pfd_delete_transaction_step(std::move(st));
      });
}

//------------------------------------------------------------------------------
// nnef_pfd_partial_pull. The sync handler holds
// shared_lock(m_nnef_pfd_transactions_mutex) across all N UDR GETs. A lock
// cannot span async hops, so this version instead:
//
//   1. SNAPSHOTS the iteration set under the lock, applying the requested_ids
//      filter and copying {app_id, fallback} per app, where fallback is the
//      stored app_data.
//   2. RELEASES the lock.
//   3. Runs the read cursor. Each step fires udr_get_pfd_data_at_async, and
//      the continuation pushes the parsed body on a strict 200, else the
//      fallback, with ["applicationId"] = app_id.
//   4. Sends 200 with the collected array from the final step.
//
// No nef_app lock is held across any async hop.
void nef_app::nnef_pfd_partial_pull(
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_nf(NEF_SERVICE_PFD_MANAGEMENT, sink)) return;

  std::vector<std::string> requested_ids;
  if (body.contains("appIds") && body["appIds"].is_array()) {
    for (const auto& v : body["appIds"]) {
      if (v.is_string()) requested_ids.push_back(v.get<std::string>());
    }
  }

  auto st  = std::make_shared<PfdPullChain>();
  st->sink = std::move(sink);
  // SNAPSHOT the iteration set under the lock, then RELEASE before any GET.
  {
    std::shared_lock lock(m_nnef_pfd_transactions_mutex);
    for (const auto& [trans_id, transaction] : m_nnef_pfd_transactions) {
      if (!transaction.contains("applications") ||
          !transaction["applications"].is_object())
        continue;
      for (const auto& [app_id, app_data] :
           transaction["applications"].items()) {
        if (!requested_ids.empty()) {
          bool found = false;
          for (const auto& f : requested_ids)
            if (f == app_id) {
              found = true;
              break;
            }
          if (!found) continue;
        }
        st->apps.emplace_back(app_id, app_data);  // fallback = stored app_data
      }
    }
  }  // lock released here — the GET cursor runs lock-free

  if (st->apps.empty()) {
    // Nothing to query — return the empty array immediately (no southbound).
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(http_status_code::OK, st->result.dump());
  }

  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    // No UDR to refresh from, so serve every entry out of the stored app_data
    // instead — exactly what the sync path does when the read fails.
    Logger::nef_app().warn(
        "NNEF_PFD partial-pull: UDR discovery failed; returning cached PFD "
        "data");
    for (auto& [app_id, fallback] : st->apps) {
      nlohmann::json entry   = fallback;
      entry["applicationId"] = app_id;
      st->result.push_back(entry);
    }
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(http_status_code::OK, st->result.dump());
  }
  clear_request_bearer_token();

  nnef_pfd_partial_pull_step(std::move(st));
}

//------------------------------------------------------------------------------
void nef_app::nnef_pfd_partial_pull_step(std::shared_ptr<PfdPullChain> st) {
  if (st->idx == st->apps.size()) {
    response_sink s = std::move(st->sink);
    return s(http_status_code::OK, st->result.dump());
  }
  const std::string app_id      = st->apps[st->idx].first;
  const nlohmann::json fallback = st->apps[st->idx].second;
  m_nef_client->udr_get_pfd_data_at_async(
      st->udr_ep, app_id,
      [this, st, app_id, fallback](oai::sba::response r) mutable {
        // Best-effort per app, reproducing the sync path exactly:
        //
        //   entry = (udr_code == OK) ? udr_result : app_data
        //
        // The sync udr_get_pfd_data sets udr_result to the parsed body on a
        // 200, and to an empty object {} when that body is empty or
        // unparseable. Note it compares strictly against OK, not 2xx.
        nlohmann::json entry;
        if (r.status_code == http_status_code::OK) {
          entry = nlohmann::json::object();  // matches sync result default
          if (!r.body.empty()) {
            try {
              entry = nlohmann::json::parse(r.body);
            } catch (...) {
              Logger::nef_app().warn(
                  "Failed to parse UDR PFD GET response body for app=%s",
                  app_id.c_str());
              entry = nlohmann::json::object();
            }
          }
        } else {
          Logger::nef_app().debug(
              "NNEF_PFD partial-pull: using cached PFD data for app=%s "
              "(http=%d)",
              app_id.c_str(), r.status_code);
          entry = fallback;
        }
        entry["applicationId"] = app_id;
        st->result.push_back(entry);
        st->idx++;
        nnef_pfd_partial_pull_step(std::move(st));
      });
}

//------------------------------------------------------------------------------
// pfd_transaction_put — FATAL-500 with a southbound-only rollback.
//
// pfd_transaction_put  authorizes, validates and typed-parses pfdDatas,
//                      decides create-vs-update, and resolves UDR on the
//                      dispatcher worker.
// pfd_put_step         the PfdPutChain cursor: PUTs each app to UDR in turn.
//                      On full success it commits the local state and sends
//                      201 (create) or 200 (update).
// pfd_put_rollback /   on a failure at step k, issue compensating DELETEs over
// pfd_rollback_step    committed[0..k-1] in REVERSE order, then send 500 from
//                      the last rollback continuation. This mirrors the
//                      PfdRollbackTracker.
void nef_app::pfd_transaction_put(
    const std::string& scs_as_id, const std::string& trans_id,
    const nlohmann::json& body, const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_af(scs_as_id, NEF_SERVICE_PFD_MANAGEMENT, sink)) {
    return;
  }
  if (!body.contains("pfdDatas")) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(
            http_status_code::BAD_REQUEST, "Missing required field: pfdDatas")
            .dump());
  }
  if (!body["pfdDatas"].is_object()) {
    clear_request_bearer_token();
    return sink(
        http_status_code::UNPROCESSABLE_ENTITY,
        make_problem_detail(
            http_status_code::UNPROCESSABLE_ENTITY,
            "pfdDatas: must be an object")
            .dump());
  }

  auto st       = std::make_shared<PfdPutChain>();
  st->scs_as_id = scs_as_id;
  st->trans_id  = trans_id;
  st->body      = body;
  st->sink      = std::move(sink);

  // Typed parse + validate each app's PFD data.
  for (auto& [app_id, app_json] : body["pfdDatas"].items()) {
    oai::_3gpp::model::PfdDataForApp app;
    try {
      from_json(app_json, app);
      app.validate();
    } catch (const nlohmann::json::exception& e) {
      response_sink s = std::move(st->sink);
      clear_request_bearer_token();
      return s(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST,
              "pfdDatas." + app_id + ": " + e.what())
              .dump());
    } catch (const oai::_3gpp::model::helpers::ValidationException& e) {
      response_sink s = std::move(st->sink);
      clear_request_bearer_token();
      return s(
          http_status_code::UNPROCESSABLE_ENTITY,
          make_problem_detail(
              http_status_code::UNPROCESSABLE_ENTITY,
              "pfdDatas." + app_id + ": " + e.what())
              .dump());
    } catch (const std::exception& e) {
      // e.g. std::invalid_argument thrown by a generated enum from_json on an
      // unrecognised value. Treat as a malformed request body (400) rather
      // than letting it escape and abort the process.
      response_sink s = std::move(st->sink);
      clear_request_bearer_token();
      return s(
          http_status_code::BAD_REQUEST,
          make_problem_detail(
              http_status_code::BAD_REQUEST,
              "pfdDatas." + app_id + ": " + e.what())
              .dump());
    }
    nlohmann::json pfd_json;
    to_json(pfd_json, app);
    st->apps.emplace_back(app_id, std::move(pfd_json));
    st->app_map[app_id] = std::move(app);
  }

  // Decide create-vs-update before any UDR write.
  {
    std::shared_lock lock(m_pfd_mutex);
    st->is_create =
        (m_pfd_trans_sessions.find(trans_id) == m_pfd_trans_sessions.end());
  }

  // Resolve UDR HERE, on the dispatcher worker.
  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    // No UDR write happened yet, so there is nothing to roll back. FATAL-500,
    // matching the sync path, which aborts the transaction with 500 on a UDR
    // write failure.
    Logger::nef_app().error(
        "PFD_TX put: UDR discovery failed for trans_id=%s", trans_id.c_str());
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(
        http_status_code::INTERNAL_SERVER_ERROR,
        make_problem_detail(
            http_status_code::INTERNAL_SERVER_ERROR,
            "PFD transaction aborted: UDR not available")
            .dump());
  }
  clear_request_bearer_token();

  pfd_put_step(std::move(st));
}

//------------------------------------------------------------------------------
void nef_app::pfd_put_step(std::shared_ptr<PfdPutChain> st) {
  if (st->idx == st->apps.size()) {
    // All UDR writes succeeded — commit local state.
    {
      const std::lock_guard<std::shared_mutex> lock(m_pfd_mutex);
      m_pfd_trans_sessions[st->trans_id] = std::move(st->app_map);
      m_pfd_trans2scs_id[st->trans_id]   = st->scs_as_id;
    }
    nlohmann::json resp = st->body;
    resp["transId"]     = st->trans_id;
    const int code =
        st->is_create ? http_status_code::CREATED : http_status_code::OK;
    nef_audit::log(
        st->is_create ? "CREATE" : "UPDATE", "PFD_TX", st->scs_as_id,
        st->trans_id, code);
    response_sink s = std::move(st->sink);
    return s(code, resp.dump());
  }
  const std::string app_id       = st->apps[st->idx].first;
  const nlohmann::json& pfd_json = st->apps[st->idx].second;
  m_nef_client->udr_put_pfd_data_at_async(
      st->udr_ep, app_id, pfd_json,
      [this, st, app_id](oai::sba::response r) mutable {
        if (!sbi_ok(r)) {
          Logger::nef_app().error(
              "F1.10: UDR PFD write failed for app '%s' in trans '%s'; rolling "
              "back %zu committed app(s)",
              app_id.c_str(), st->trans_id.c_str(), st->committed.size());
          return pfd_put_rollback(std::move(st), app_id);
        }
        st->committed.push_back(app_id);
        st->idx++;
        pfd_put_step(std::move(st));
      });
}

//------------------------------------------------------------------------------
void nef_app::pfd_put_rollback(
    std::shared_ptr<PfdPutChain> st, const std::string& failed_app) {
  // Async compensating DELETEs over committed[0..k-1] in REVERSE order; the
  // last rollback continuation sends the 500. Mirrors
  // PfdRollbackTracker::execute.
  //
  // No local state was committed yet, so the rollback is southbound only.
  pfd_rollback_step(std::move(st), st->committed.size(), failed_app);
}

void nef_app::pfd_rollback_step(
    std::shared_ptr<PfdPutChain> st, std::size_t remaining,
    const std::string& failed_app) {
  if (remaining == 0) {
    // Rollback complete — abort the transaction with 500.
    nlohmann::json pd = make_problem_detail(
        http_status_code::INTERNAL_SERVER_ERROR,
        "PFD transaction aborted: UDR write failed for app " + failed_app);
    response_sink s = std::move(st->sink);
    return s(http_status_code::INTERNAL_SERVER_ERROR, pd.dump());
  }
  const std::string rid = st->committed[remaining - 1];  // reverse order
  m_nef_client->udr_delete_pfd_data_at_async(
      st->udr_ep, rid,
      [this, st, remaining, failed_app, rid](oai::sba::response r) mutable {
        if (!sbi_ok(r)) {
          // UDR may retain orphan data, but the request still aborts with 500.
          Logger::nef_app().error(
              "F1.10: Rollback delete failed for app '%s' in trans '%s'",
              rid.c_str(), st->trans_id.c_str());
        }
        pfd_rollback_step(std::move(st), remaining - 1, failed_app);
      });
}

//------------------------------------------------------------------------------
// nnef_pfd_put_transaction — the Nnef twin of pfd_transaction_put: FATAL-500
// with a southbound-only rollback, plus a post-commit cleanup phase.
//
// nnef_pfd_put_transaction  authorizes, extracts and normalizes the
//                           applications, decides create-vs-update, computes
//                           the phase-C removed-app set, and resolves UDR on
//                           the dispatcher worker.
// nnef_put_step             the NnefPutChain cursor: PUTs each app to UDR.
// nnef_put_rollback /       on a failure, compensating DELETEs over
// nnef_rollback_step        committed[0..k-1] in REVERSE order, then 500.
// nnef_put_after_commit     commits the local state, sends the success
//                           response, notifies the PFD subscribers, and fires
//                           the best-effort removed-app deletes.
void nef_app::nnef_pfd_put_transaction(
    const std::string& transaction_id, const nlohmann::json& body,
    const std::string& token, response_sink sink) {
  set_request_bearer_token(token);
  if (reject_unauthorized_nf(NEF_SERVICE_PFD_MANAGEMENT, sink)) return;
  nlohmann::json applications;
  std::string error_detail;
  if (!extract_nnef_pfd_transaction_apps(body, applications, error_detail)) {
    clear_request_bearer_token();
    return sink(
        http_status_code::BAD_REQUEST,
        make_problem_detail(http_status_code::BAD_REQUEST, error_detail)
            .dump());
  }

  auto st            = std::make_shared<NnefPutChain>();
  st->transaction_id = transaction_id;
  st->applications   = applications;
  st->transaction =
      make_nnef_pfd_transaction(transaction_id, body, applications);
  st->sink = std::move(sink);
  for (const auto& [app_id, _] : applications.items())
    st->app_ids.push_back(app_id);

  // Decide create-vs-update and compute the phase-C removed-app set, both
  // under the mutex. The removed apps are the ones the prior transaction
  // carried that are absent from the new set.
  {
    std::shared_lock lock(m_nnef_pfd_transactions_mutex);
    auto it       = m_nnef_pfd_transactions.find(transaction_id);
    st->is_create = (it == m_nnef_pfd_transactions.end());
    if (!st->is_create) {
      const nlohmann::json& prev = it->second;
      if (prev.contains("applications") && prev["applications"].is_object()) {
        for (const auto& [app_id, _] : prev["applications"].items()) {
          if (!applications.contains(app_id))
            st->removed_apps.push_back(app_id);
        }
      }
    }
  }

  // Resolve UDR HERE, on the dispatcher worker.
  if (!m_nef_client->discover_nf(nf_type_t::NF_TYPE_UDR, st->udr_ep)) {
    // No UDR write happened yet, so there is nothing to roll back. FATAL-500.
    Logger::nef_app().error(
        "NNEF_PFD_TX put: UDR discovery failed for trans_id=%s",
        transaction_id.c_str());
    response_sink s = std::move(st->sink);
    clear_request_bearer_token();
    return s(
        http_status_code::INTERNAL_SERVER_ERROR,
        make_problem_detail(
            http_status_code::INTERNAL_SERVER_ERROR,
            "PFD transaction aborted: UDR not available")
            .dump());
  }
  clear_request_bearer_token();

  nnef_put_step(std::move(st));
}

// PUT each app to UDR. A failure goes to nnef_put_rollback, which ends in 500.
// Once every app is written, control passes to nnef_put_after_commit.
void nef_app::nnef_put_step(std::shared_ptr<NnefPutChain> st) {
  if (st->idx == st->app_ids.size()) {
    return nnef_put_after_commit(std::move(st));
  }
  const std::string app_id      = st->app_ids[st->idx];
  const nlohmann::json app_body = st->applications[app_id];
  m_nef_client->udr_put_pfd_data_at_async(
      st->udr_ep, app_id, app_body,
      [this, st, app_id](oai::sba::response r) mutable {
        if (!sbi_ok(r)) {
          Logger::nef_app().error(
              "UDR PFD write failed for Nnef app '%s' in trans '%s'; rolling "
              "back %zu committed app(s)",
              app_id.c_str(), st->transaction_id.c_str(), st->committed.size());
          return nnef_put_rollback(std::move(st), app_id);
        }
        st->committed.push_back(app_id);
        st->idx++;
        nnef_put_step(std::move(st));
      });
}

// Compensating DELETEs over committed[0..k-1] in REVERSE order, ending in
// FATAL-500. Mirrors PfdRollbackTracker. Southbound only: no local state was
// committed yet.
void nef_app::nnef_put_rollback(
    std::shared_ptr<NnefPutChain> st, const std::string& failed_app) {
  nnef_rollback_step(std::move(st), st->committed.size(), failed_app);
}

void nef_app::nnef_rollback_step(
    std::shared_ptr<NnefPutChain> st, std::size_t remaining,
    const std::string& failed_app) {
  if (remaining == 0) {
    nlohmann::json pd = make_problem_detail(
        http_status_code::INTERNAL_SERVER_ERROR,
        "PFD transaction aborted: UDR write failed for app " + failed_app);
    response_sink s = std::move(st->sink);
    return s(http_status_code::INTERNAL_SERVER_ERROR, pd.dump());
  }
  const std::string rid = st->committed[remaining - 1];  // reverse order
  m_nef_client->udr_delete_pfd_data_at_async(
      st->udr_ep, rid,
      [this, st, remaining, failed_app, rid](oai::sba::response r) mutable {
        if (!sbi_ok(r)) {
          Logger::nef_app().error(
              "Rollback delete failed for Nnef app '%s' in trans '%s'",
              rid.c_str(), st->transaction_id.c_str());
        }
        nnef_rollback_step(std::move(st), remaining - 1, failed_app);
      });
}

// Commit the local state and SEND the success response, then do the
// post-commit work that must not be allowed to change it.
void nef_app::nnef_put_after_commit(std::shared_ptr<NnefPutChain> st) {
  // Commit local state.
  {
    const std::lock_guard<std::shared_mutex> lock(
        m_nnef_pfd_transactions_mutex);
    m_nnef_pfd_transactions[st->transaction_id] = st->transaction;
  }

  const int code =
      st->is_create ? http_status_code::CREATED : http_status_code::OK;
  nef_audit::log(
      st->is_create ? "CREATE" : "UPDATE", "NNEF_PFD_TX", "",
      st->transaction_id, code);

  // SEND THE SUCCESS RESPONSE NOW. The sink is exactly-once: after this call
  // it is spent and must never be touched again.
  st->sink(code, st->transaction.dump());

  // Notify SBI PFD subscribers — non-blocking, enqueued on
  // m_notification_pool.
  if (st->transaction.contains("applications") &&
      st->transaction["applications"].is_object()) {
    for (const auto& [app_id, app_data] :
         st->transaction["applications"].items()) {
      notify_nnef_pfd_subscribers("PFD_CHANGE", app_id, app_data);
    }
  }

  // POST-COMMIT best-effort cleanup of the removed apps: fire-and-forget async
  // DELETEs. Failures are warn-only and ignored, nothing is rolled back, and
  // the response has already gone out.
  //
  // These continuations capture value copies only — tid and app_id, never st
  // and never the sink — so the spent deferred handle is never re-touched.
  const std::string udr_ep = st->udr_ep;  // copy out before st is released
  const std::string tid    = st->transaction_id;
  for (const std::string& app_id : st->removed_apps) {  // empty on create
    m_nef_client->udr_delete_pfd_data_at_async(
        udr_ep, app_id, [tid, app_id](oai::sba::response r) {
          if (!(r.status_code >= 200 && r.status_code < 300)) {
            Logger::nef_app().warn(
                "UDR PFD delete failed for removed Nnef_PFDmanagement app: %s "
                "in transaction: %s",
                app_id.c_str(), tid.c_str());
          }
        });
  }
  // st (and its now-spent sink) drops when this frame returns.
}
