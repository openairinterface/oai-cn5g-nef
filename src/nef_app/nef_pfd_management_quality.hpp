/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <string>
#include <vector>
#include <rfl/Generic.hpp>

namespace oai::nef::app {

// Application-level filtering

namespace detail {
// Build a per-app entry: copy the app's pfdData object (if any) and inject the
// "appId" field.
inline rfl::Generic pfd_make_entry(
    const rfl::Generic& data, const std::string& app_id) {
  rfl::Generic::Object entry;
  if (const auto* obj = std::get_if<rfl::Generic::Object>(&data.variant())) {
    entry = *obj;
  }
  entry["appId"] = rfl::Generic(app_id);
  return rfl::Generic(std::move(entry));
}
}  // namespace detail

/**
 * Filter a JSON object (appId → pfdData) by a list of requested application
 * IDs.  Returns a JSON array, each element being the per-app PFD data with
 * an extra "appId" field injected.
 *
 * - If @p requested_ids is empty, ALL apps are returned (no filter).
 * - Unknown IDs in @p requested_ids are silently skipped (no 404 — the
 *   caller decides whether to surface errors; partial results are normal for
 *   the Nnef_PFDmanagement_Fetch service operation).
 */
inline rfl::Generic pfd_filter_applications(
    const rfl::Generic& apps_map,
    const std::vector<std::string>& requested_ids) {
  rfl::Generic::Array result;
  const auto* apps_obj = std::get_if<rfl::Generic::Object>(&apps_map.variant());
  if (!apps_obj) return rfl::Generic(std::move(result));

  if (requested_ids.empty()) {
    // Return all apps without a filter.
    for (const auto& [app_id, data] : *apps_obj) {
      result.push_back(detail::pfd_make_entry(data, app_id));
    }
    return rfl::Generic(std::move(result));
  }

  for (const auto& req_id : requested_ids) {
    if (auto r = apps_obj->get(req_id)) {
      result.push_back(detail::pfd_make_entry(r.value(), req_id));
    }
  }
  return rfl::Generic(std::move(result));
}

// Subscription helpers

/**
 * Validate a PFD-management subscription body.
 *
 * Required field: "notifUri" — non-empty string.
 * Returns an error description; empty string means valid.
 */
inline std::string validate_nnef_pfd_subscription(const rfl::Generic& body) {
  if (const auto* obj = std::get_if<rfl::Generic::Object>(&body.variant())) {
    if (auto r = obj->get("notifUri")) {
      if (const auto* s = std::get_if<std::string>(&r.value().variant())) {
        if (!s->empty()) return "";
      }
    }
  }
  return "notifUri is required and must be a non-empty string";
}

/**
 * Build the canonical self-link for a PFD management subscription.
 */
inline std::string nnef_pfd_subscription_self_link(const std::string& sub_id) {
  return "/nnef-pfdmanagement/v1/subscriptions/" + sub_id;
}

/**
 * Perform a full-replace of a PFD subscription.
 *
 * Full-replace semantics: the stored state becomes exactly @p new_body
 * (not a merge/patch).  "subscriptionId" and "self" are injected.
 * Any previously stored fields that are absent from @p new_body are lost.
 */
inline rfl::Generic nnef_pfd_subscription_make(
    const std::string& sub_id, const rfl::Generic& new_body) {
  rfl::Generic::Object stored;  // full replacement — no merge
  if (const auto* obj =
          std::get_if<rfl::Generic::Object>(&new_body.variant())) {
    stored = *obj;
  }
  stored["subscriptionId"] = rfl::Generic(sub_id);
  stored["self"] = rfl::Generic(nnef_pfd_subscription_self_link(sub_id));
  return rfl::Generic(std::move(stored));
}

/**
 * Check whether a PFD subscription is interested in changes to @p app_id.
 *
 * If the subscription body contains an "applicationIds" array, the
 * subscription is considered relevant only if @p app_id is listed.
 * If "applicationIds" is absent or empty the subscription receives all
 * PFD-change events (wildcard).
 */
inline bool nnef_pfd_subscription_matches(
    const rfl::Generic& sub, const std::string& app_id) {
  const auto* obj = std::get_if<rfl::Generic::Object>(&sub.variant());
  if (!obj) return true;  // wildcard
  auto r = obj->get("applicationIds");
  if (!r) return true;  // wildcard
  const auto* ids = std::get_if<rfl::Generic::Array>(&r.value().variant());
  if (!ids || ids->empty()) return true;  // wildcard
  for (const auto& id : *ids) {
    if (const auto* s = std::get_if<std::string>(&id.variant())) {
      if (*s == app_id) return true;
    }
  }
  return false;
}

}  // namespace oai::nef::app
