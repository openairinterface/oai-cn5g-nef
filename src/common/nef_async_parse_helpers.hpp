/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
//
// Helpers that extract resource ids from southbound responses.
//
// The async nef_client wrappers return the raw response and leave parsing to
// the caller, so each cont_* continuation has to extract the id itself. The
// synchronous counterpart in nef_client.cpp does the same extraction inline.
//
// That synchronous logic is file-static inside nef_client.cpp and cannot be
// called from here, so it is repeated below. Keep the two in step when either
// changes.
//
// Header-only and free of nef_app state, so the tests can use it without
// linking nef_client.

#ifndef NEF_ASYNC_PARSE_HELPERS_HPP
#define NEF_ASYNC_PARSE_HELPERS_HPP

#include <algorithm>
#include <cctype>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "AmfCreatedEventSubscription.h"
#include "http_definitions.hpp"  // oai::nghttp2::response

namespace oai::nef::app {

// Case-insensitive header lookup.
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

// Trailing path segment of a URI, ignoring any query or fragment.
inline std::string nef_async_last_path_segment(const std::string& uri) {
  if (uri.empty()) return {};
  std::string u = uri;
  const auto q  = u.find_first_of("?#");
  if (q != std::string::npos) u = u.substr(0, q);
  while (!u.empty() && u.back() == '/') u.pop_back();
  const auto slash = u.find_last_of('/');
  return (slash == std::string::npos) ? u : u.substr(slash + 1);
}

// PCF appSessionId: the body's "appSessionId" if present, otherwise the last
// path segment of the Location header. Body first, as in the synchronous
// version.
inline std::string nef_async_parse_pcf_app_session_id(
    const oai::nghttp2::response& r) {
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

// PCF BDT-policy id: the Location header first, then the body fields
// bdtPolicyId -> bdtRefId -> bdtPolData.bdtRefId.
//
// This is the reverse of the appSessionId order above, on purpose: it matches
// the synchronous create_pcf_bdt_policy. When PCF answers with both a
// Location header and a body id, both paths then derive the same id and store
// the same value in m_bdt_id2pcf_policy_id.
inline std::string nef_async_parse_pcf_bdt_policy_id(
    const oai::nghttp2::response& r) {
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

// AMF event-subscription id from a raw create response. Prefers the typed
// AmfCreatedEventSubscription parse, then falls back to the raw fields
// subscriptionId -> eventsSubscription.subscriptionId.
inline std::string nef_async_parse_amf_sub_id(const oai::nghttp2::response& r) {
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
