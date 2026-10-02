/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

// Helpers shared by the nef_app translation units.
//
// They were file-statics in the single nef_app.cpp. Splitting that file left
// each of them with callers in more than one translation unit, so they live
// here as inline definitions -- one entity, one definition, every TU
// agreeing. Helpers used by exactly one translation unit stayed file-static
// in that file and are deliberately not here. The comments below came over
// from nef_app.cpp with them.

#ifndef FILE_NEF_APP_INTERNAL_HPP_SEEN
#define FILE_NEF_APP_INTERNAL_HPP_SEEN

#include <boost/date_time/posix_time/posix_time.hpp>
#include <chrono>
#include <ctime>
#include <string>

#include <nlohmann/json.hpp>

#include "3gpp_29.500.h"

// The reason phrase that belongs to each status code NEF answers with. One
// definition per code, so a phrase is written once here rather than retyped
// next to the code at every call site -- where nothing ever checked that the
// two agreed.
//
// A status that is absent from this table has no phrase in NEF's vocabulary:
// spell those responses with the three-argument overload, which takes the
// title explicitly.
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

// Same, with the title derived from the status code instead of retyped.
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

// Carried over from nef_app.cpp, where it headed the phase-1 section. After
// the split "every handler below" means every phase-1 entry method and its
// cont_* half, in each nef_app_*.cpp that includes this header.
// ═══════════════════════════════════════════════════════════════════════════
// Async handler split
//
// Every handler below is cut into two halves, and they all follow the same
// shape.
//
// The phase-1 entry method runs on the dispatcher worker. It does the same
// pre-southbound work its synchronous ancestor did — authorize, parse and
// validate, SSRF-check, update the local store — bookended by
// set_request_bearer_token() and clear_request_bearer_token(), and answers
// through the sink on any early return. It then copies whatever the
// continuation will need (by value: nothing may outlive the call), fires the
// southbound request, and returns. It never waits for the round trip, so the
// worker stays free.
//
// The cont_* half runs later on oai-http-io, or inline when the answer was
// already known. It holds no bearer token. It re-locks every store it
// touches, because the world may have moved on since the entry method ran.
// It applies the handler's failure policy and completes the deferred
// response with the same status and body the synchronous version would have
// produced.
// ═══════════════════════════════════════════════════════════════════════════

#endif
