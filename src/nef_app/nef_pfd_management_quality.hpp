/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

/*! \file nef_pfd_management_quality.hpp
 * \brief Header-only utilities for F3.2 — Nnef_PFDmanagement Quality
 *        Verification (SBI, TS 29.551).
 *
 * Pure logic — no dependency on the NEF library or config singleton.
 * Directly testable by unit tests without linking the full nef_lib.
 */

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace oai::nef::app {

// Application-level filtering

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
 * Validate a PFD-management subscription body.
 *
 * Required field: "notifUri" — non-empty string.
 * Returns an error description; empty string means valid.
 */
inline std::string validate_nnef_pfd_subscription(const nlohmann::json& body) {
  if (!body.contains("notifUri") || !body["notifUri"].is_string() ||
      body["notifUri"].get<std::string>().empty()) {
    return "notifUri is required and must be a non-empty string";
  }
  return "";
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
inline nlohmann::json nnef_pfd_subscription_make(
    const std::string& sub_id, const nlohmann::json& new_body) {
  nlohmann::json stored    = new_body;          // full replacement — no merge
  stored["subscriptionId"] = sub_id;
  stored["self"]           = nnef_pfd_subscription_self_link(sub_id);
  return stored;
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

}  // namespace oai::nef::app
