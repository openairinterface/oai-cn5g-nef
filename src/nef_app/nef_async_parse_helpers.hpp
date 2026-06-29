/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
//
// NEF true-async continuation parse helpers
// (plan 20260625-nef-true-async-all-apis, §A).
//
// The *_async / *_at_async nef_client wrappers deliberately do NOT parse IDs
// out of the southbound response — they hand the raw oai::http::response to the
// continuation. So each cont_* reproduces the ID-parse its SYNCHRONOUS
// nef_client twin did inline. Those sync parses live in TU-private file-static
// helpers in nef_client.cpp (get_header_case_insensitive /
// extract_last_path_segment, and the create_pcf_policy_auth /
// subscribe_amf_event_exposure body logic), so they are re-expressed here,
// byte-for-byte equivalent, and shared between nef_app.cpp (the production
// continuations) and the §F.1 differential test so the test pins the EXACT
// parse the continuation uses.
//
// PURE + header-only: no nef_app state, no nef_client link dependency —
// testable directly (mirrors the split rationale of
// nef_sbi_response_policy.hpp).

#ifndef NEF_ASYNC_PARSE_HELPERS_HPP
#define NEF_ASYNC_PARSE_HELPERS_HPP

#include <algorithm>
#include <cctype>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "AmfCreatedEventSubscription.h"
#include "http_definitions.hpp"  // oai::http::response

namespace oai::nef::app {

// Case-insensitive header lookup — mirrors nef_client.cpp:109-129
// (get_header_case_insensitive).
inline std::string nef_async_header_ci(
    const std::map<std::string, std::string>& headers, const std::string& key) {
  for (const auto& kv : headers) {
    if (kv.first.size() == key.size() &&
        std::equal(
            kv.first.begin(), kv.first.end(), key.begin(),
            [](unsigned char a, unsigned char b) {
              return std::tolower(a) == std::tolower(b);
            })) {
      return kv.second;
    }
  }
  return {};
}

// Trailing path segment of a URI — mirrors nef_client.cpp:131-145
// (extract_last_path_segment).
inline std::string nef_async_last_path_segment(const std::string& uri) {
  if (uri.empty()) return {};
  std::string u = uri;
  const auto q  = u.find_first_of("?#");
  if (q != std::string::npos) u = u.substr(0, q);
  while (!u.empty() && u.back() == '/') u.pop_back();
  const auto slash = u.find_last_of('/');
  return (slash == std::string::npos) ? u : u.substr(slash + 1);
}

// PCF appSessionId from a raw create-app-session response — mirrors
// create_pcf_policy_auth (nef_client.cpp:933-945): prefer the JSON
// "appSessionId" field, else the last path segment of the Location header.
inline std::string nef_async_parse_pcf_app_session_id(
    const oai::http::response& r) {
  std::string id;
  try {
    nlohmann::json j = nlohmann::json::parse(r.body);
    id               = j.value("appSessionId", "");
  } catch (...) {
  }
  if (id.empty()) {
    id =
        nef_async_last_path_segment(nef_async_header_ci(r.headers, "Location"));
  }
  return id;
}

// PCF BDT-policy id from a raw create-bdt-policy response — mirrors
// create_pcf_bdt_policy (nef_client.cpp:1166-1179): Location-header last-path
// segment FIRST, and only if that is empty fall back to the body fields
// bdtPolicyId → bdtRefId → bdtPolData.bdtRefId. This precedence differs from
// nef_async_parse_pcf_app_session_id (which is appSessionId-body-first) — BDT
// MUST be Location-first to match the sync twin, otherwise a body-only PCF
// response leaves m_bdt_id2pcf_policy_id unwired.
inline std::string nef_async_parse_pcf_bdt_policy_id(
    const oai::http::response& r) {
  std::string id =
      nef_async_last_path_segment(nef_async_header_ci(r.headers, "Location"));
  if (id.empty() && !r.body.empty()) {
    try {
      nlohmann::json j = nlohmann::json::parse(r.body);
      id               = j.value("bdtPolicyId", "");
      if (id.empty()) id = j.value("bdtRefId", "");
      if (id.empty() && j.contains("bdtPolData")) {
        id = j["bdtPolData"].value("bdtRefId", "");
      }
    } catch (...) {
    }
  }
  return id;
}

// AMF event-subscription id from a raw create response — mirrors
// subscribe_amf_event_exposure (nef_client.cpp:619-642).
inline std::string nef_async_parse_amf_sub_id(const oai::http::response& r) {
  std::string amf_sub_id;
  try {
    nlohmann::json j = nlohmann::json::parse(r.body);
    try {
      oai::_3gpp::model::AmfCreatedEventSubscription created;
      from_json(j, created);
      amf_sub_id = created.getSubscriptionId();
    } catch (...) {
      amf_sub_id = j.value("subscriptionId", "");
      if (amf_sub_id.empty() && j.contains("eventsSubscription")) {
        amf_sub_id = j["eventsSubscription"].value("subscriptionId", "");
      }
    }
  } catch (...) {
  }
  return amf_sub_id;
}

}  // namespace oai::nef::app

#endif  // NEF_ASYNC_PARSE_HELPERS_HPP
