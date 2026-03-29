/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

/**
 * @file nef_audit_record.hpp
 * @brief Audit record builder (logger-free, testable).
 *
 * Separated from nef_audit_log.hpp so unit tests can include this without
 * pulling in spdlog/logger.hpp.
 */

#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace oai::nef::app {

class nef_audit_record {
 public:
  /// Build one structured JSON audit record string.
  /// @param op        Operation: "CREATE", "UPDATE", "DELETE", "PATCH"
  /// @param resource  Resource type: "TI", "QOS", "BDT", "PFD", "EE", "ME"
  /// @param af_id     AF / SCS-AS identifier (may be empty)
  /// @param res_id    Resource identifier (may be empty for collection ops)
  /// @param http_code HTTP response code produced by the operation
  static std::string make_record(
      const std::string& op,
      const std::string& resource,
      const std::string& af_id,
      const std::string& res_id,
      int                http_code) {
    std::ostringstream oss;
    oss << "{"
        << R"("ts":")"       << utc_now()           << R"(",)"
        << R"("op":")"       << escape_json(op)       << R"(",)"
        << R"("resource":")" << escape_json(resource) << R"(",)"
        << R"("af_id":")"    << escape_json(af_id)    << R"(",)"
        << R"("res_id":")"   << escape_json(res_id)   << R"(",)"
        << R"("http_code":)" << http_code
        << "}";
    return oss.str();
  }

 private:
  /// Returns current UTC time as ISO-8601 string: YYYY-MM-DDTHH:MM:SSZ
  static std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
  }

  /// Minimal JSON string escaping (quote and backslash only).
  static std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
      if (c == '"')       { out += "\\\""; }
      else if (c == '\\') { out += "\\\\"; }
      else                { out += c; }
    }
    return out;
  }
};

}  // namespace oai::nef::app
