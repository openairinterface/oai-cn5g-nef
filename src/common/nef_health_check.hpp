/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace oai::nef::app::nef_health_check {

/**
 * The health-check body, and its status code: 503 while the server is
 * draining for a graceful shutdown, 200 otherwise.
 */
inline std::string make_response(
    bool draining, const std::string& instance_id, int uptime_seconds,
    int& http_code) {
  nlohmann::json j;
  if (draining) {
    http_code    = 503;
    j["status"]  = "draining";
    j["nf_type"] = "NEF";
  } else {
    http_code           = 200;
    j["status"]         = "ok";
    j["nf_type"]        = "NEF";
    j["instance_id"]    = instance_id;
    j["uptime_seconds"] = uptime_seconds >= 0 ? uptime_seconds : 0;
    j["draining"]       = false;
  }
  return j.dump();
}

}  // namespace oai::nef::app::nef_health_check
