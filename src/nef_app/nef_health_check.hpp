/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

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
    bool draining,
    const std::string& instance_id,
    int uptime_seconds,
    int& http_code) {
  nlohmann::json j;
  if (draining) {
    http_code    = 503;
    j["status"]  = "draining";
    j["nf_type"] = "NEF";
  } else {
    http_code              = 200;
    j["status"]            = "ok";
    j["nf_type"]           = "NEF";
    j["instance_id"]       = instance_id;
    j["uptime_seconds"]    = uptime_seconds >= 0 ? uptime_seconds : 0;
    j["draining"]          = false;
  }
  return j.dump();
}

}  // namespace oai::nef::app::nef_health_check
