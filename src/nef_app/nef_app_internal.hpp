/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// Helpers shared by the nef_app translation units.
//
// Each helper here has callers in more than one nef_app_*.cpp file, so it is
// defined inline in this header: one definition that every translation unit
// shares. A helper used by only one translation unit stays file-static in
// that file and is deliberately not here.

#ifndef FILE_NEF_APP_INTERNAL_HPP_SEEN
#define FILE_NEF_APP_INTERNAL_HPP_SEEN

#include <boost/date_time/posix_time/posix_time.hpp>
#include <chrono>
#include <ctime>
#include <string>

#include <nlohmann/json.hpp>

#include "3gpp_29.500.h"

// The reason phrase for each status code NEF answers with. Each phrase is
// written once here instead of being retyped at every call site, where
// nothing would check that the phrase matches the code.
//
// A status missing from this table has no phrase in NEF: build those
// responses with the three-argument make_problem_detail overload, which takes
// the title explicitly.
constexpr const char* http_reason_phrase(int status) {
  switch (status) {
    case http_status_code::BAD_REQUEST:
      return "Bad Request";
    case http_status_code::FORBIDDEN:
      return "Forbidden";
    case http_status_code::NOT_FOUND:
      return "Not Found";
    case http_status_code::UNPROCESSABLE_ENTITY:
      return "Unprocessable Entity";
    case http_status_code::INTERNAL_SERVER_ERROR:
      return "Internal Server Error";
    case http_status_code::BAD_GATEWAY:
      return "Bad Gateway";
    default:
      return "";
  }
}

//------------------------------------------------------------------------------
// RFC 7807 Problem Detail helper
inline nlohmann::json make_problem_detail(
    int status, const std::string& title, const std::string& detail,
    const std::string& instance = "") {
  nlohmann::json pd;
  pd["type"]   = "about:blank";
  pd["title"]  = title;
  pd["status"] = status;
  pd["detail"] = detail;
  if (!instance.empty()) pd["instance"] = instance;
  return pd;
}

// Same, with the title taken from http_reason_phrase(status).
inline nlohmann::json make_problem_detail(
    int status, const std::string& detail) {
  return make_problem_detail(status, http_reason_phrase(status), detail);
}

//------------------------------------------------------------------------------
inline bool parse_monitor_expire_time(
    const std::string& value,
    std::chrono::system_clock::time_point& expire_time) {
  std::string normalized = value;
  if (normalized.empty()) return false;

  // Accept RFC3339 UTC form and trim fractional seconds/timezone offsets.
  auto dot_pos = normalized.find('.');
  if (dot_pos != std::string::npos) {
    normalized = normalized.substr(0, dot_pos);
  }

  auto plus_pos = normalized.find('+', 19);
  if (plus_pos != std::string::npos) {
    normalized = normalized.substr(0, plus_pos);
  }

  auto minus_pos = normalized.find('-', 19);
  if (minus_pos != std::string::npos) {
    normalized = normalized.substr(0, minus_pos);
  }

  if (!normalized.empty() && normalized.back() == 'Z') {
    normalized.pop_back();
  }

  try {
    auto pt       = boost::posix_time::from_iso_extended_string(normalized);
    std::tm tm    = boost::posix_time::to_tm(pt);
    const auto ts = timegm(&tm);
    if (ts < 0) return false;
    expire_time = std::chrono::system_clock::from_time_t(ts);
    return true;
  } catch (...) {
    return false;
  }
}

//==============================================================================
// Async handler split
//
// Each async request handler in the nef_app_*.cpp files that include this
// header is split into two halves, and all of them follow the same shape.
//
// The entry method runs on the dispatcher worker. It does the work that comes
// before the southbound call (authorize, parse and validate, SSRF-check,
// update the local store) between set_request_bearer_token() and
// clear_request_bearer_token(), and answers through the sink on any early
// return. It then copies whatever the continuation will need (by value:
// nothing may outlive the call), sends the southbound request, and returns.
// It never waits for the reply, so the worker stays free.
//
// The cont_* half runs later on oai-http-io, or inline when the answer is
// already known. It holds no bearer token. It locks again every store it
// touches, because the state may have changed since the entry method ran. It
// applies the handler's failure policy and completes the deferred response.
//==============================================================================

#endif
