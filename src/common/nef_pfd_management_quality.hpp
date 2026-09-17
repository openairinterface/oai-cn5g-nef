/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "PfdSubscription.h"

namespace oai::nef::app {

// Application-level filtering

/**
 * Filter a JSON map of appId → pfdData down to the requested application IDs.
 *
 * Returns a JSON array. Each element is one app's PFD data with its "appId"
 * injected as an extra field.
 *
 * Partial results are normal for the Nnef_PFDmanagement_Fetch service
 * operation, so nothing here is treated as an error:
 * - an empty @p requested_ids means no filter, and every app is returned;
 * - unknown IDs in @p requested_ids are skipped silently, with no 404. The
 *   caller decides whether to surface anything.
 */
inline nlohmann::json pfd_filter_applications(
    const nlohmann::json& apps_map,
    const std::vector<std::string>& requested_ids) {
  nlohmann::json result = nlohmann::json::array();
  if (!apps_map.is_object()) return result;

  if (requested_ids.empty()) {
    // Return all apps without a filter.
    for (const auto& [app_id, data] : apps_map.items()) {
      nlohmann::json entry = data;
      entry["appId"]       = app_id;
      result.push_back(entry);
    }
    return result;
  }

  for (const auto& req_id : requested_ids) {
    if (apps_map.contains(req_id)) {
      nlohmann::json entry = apps_map[req_id];
      entry["appId"]       = req_id;
      result.push_back(entry);
    }
  }
  return result;
}

// Subscription helpers

/**
 * Validate a PFD-management subscription body. The only required field is
 * "notifUri", a non-empty string.
 *
 * Returns an error description, or an empty string when the body is valid.
 */
inline std::string validate_nnef_pfd_subscription(const nlohmann::json& body) {
  if (!body.contains("notifUri") || !body["notifUri"].is_string() ||
      body["notifUri"].get<std::string>().empty()) {
    return "notifUri is required and must be a non-empty string";
  }
  return "";
}

/// The canonical self-link for a PFD-management subscription.
inline std::string nnef_pfd_subscription_self_link(const std::string& sub_id) {
  return "/nnef-pfdmanagement/v1/subscriptions/" + sub_id;
}

/**
 * Full-replace of a PFD subscription: the stored state becomes exactly
 * @p new_body, with "subscriptionId" and "self" injected.
 *
 * This replaces, it does not merge or patch. Any previously stored field that
 * @p new_body omits is lost.
 */
inline nlohmann::json nnef_pfd_subscription_make(
    const std::string& sub_id, const nlohmann::json& new_body) {
  nlohmann::json stored    = new_body;  // full replacement — no merge
  stored["subscriptionId"] = sub_id;
  stored["self"]           = nnef_pfd_subscription_self_link(sub_id);
  return stored;
}

/**
 * Whether a PFD subscription wants to hear about changes to @p app_id.
 *
 * A non-empty "applicationIds" array in the subscription body acts as a
 * filter: the subscription matches only the IDs it lists. If the array is
 * absent or empty the subscription is a wildcard and receives every
 * PFD-change event.
 */
inline bool nnef_pfd_subscription_matches(
    const nlohmann::json& sub, const std::string& app_id) {
  if (!sub.contains("applicationIds") || !sub["applicationIds"].is_array() ||
      sub["applicationIds"].empty()) {
    return true;  // wildcard
  }
  for (const auto& id : sub["applicationIds"]) {
    if (id.is_string() && id.get<std::string>() == app_id) return true;
  }
  return false;
}

// Same rule, for a stored PfdSubscription object.
inline bool nnef_pfd_subscription_matches(
    const oai::_3gpp::model::PfdSubscription& sub, const std::string& app_id) {
  if (!sub.applicationIdsIsSet() || sub.getApplicationIds().empty()) {
    return true;  // wildcard — no filter means all apps
  }
  for (const auto& id : sub.getApplicationIds()) {
    if (id == app_id) return true;
  }
  return false;
}

}  // namespace oai::nef::app
