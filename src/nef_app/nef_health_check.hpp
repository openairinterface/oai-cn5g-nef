/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <chrono>
#include <string>

#include <rfl/json.hpp>

namespace oai::nef::app::nef_health_check {

/**
 * Build the health-check response body and select the HTTP status code.
 *
 * @param draining        True when the server is in graceful-shutdown mode.
 * @param instance_id     NEF NF instance UUID (may be empty).
 * @param uptime_seconds  Non-negative seconds since server start.
 * @param[out] http_code  Set to 503 when draining, else 200.
 * @return                JSON string for the response body.
 */
inline std::string make_response(
    bool draining, const std::string& instance_id, int uptime_seconds,
    int& http_code) {
  rfl::Generic::Object j;
  if (draining) {
    http_code    = 503;
    j["status"]  = rfl::Generic(std::string("draining"));
    j["nf_type"] = rfl::Generic(std::string("NEF"));
  } else {
    http_code        = 200;
    j["status"]      = rfl::Generic(std::string("ok"));
    j["nf_type"]     = rfl::Generic(std::string("NEF"));
    j["instance_id"] = rfl::Generic(instance_id);
    j["uptime_seconds"] =
        rfl::Generic(int64_t(uptime_seconds >= 0 ? uptime_seconds : 0));
    j["draining"] = rfl::Generic(false);
  }
  return rfl::json::write(rfl::Generic(std::move(j)));
}

}  // namespace oai::nef::app::nef_health_check
