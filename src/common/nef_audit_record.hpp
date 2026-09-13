/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
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
  /// One structured JSON audit record. op is "CREATE", "UPDATE", "DELETE"
  /// or "PATCH"; resource is "TI", "QOS", "BDT", "PFD", "EE" or "ME". Both
  /// af_id and res_id may be empty — the latter for collection operations.
  static std::string make_record(
      const std::string& op, const std::string& resource,
      const std::string& af_id, const std::string& res_id, int http_code) {
    std::ostringstream oss;
    oss << "{"
        << R"("ts":")" << utc_now() << R"(",)"
        << R"("op":")" << escape_json(op) << R"(",)"
        << R"("resource":")" << escape_json(resource) << R"(",)"
        << R"("af_id":")" << escape_json(af_id) << R"(",)"
        << R"("res_id":")" << escape_json(res_id) << R"(",)"
        << R"("http_code":)" << http_code << "}";
    return oss.str();
  }

 private:
  /// Current UTC time as YYYY-MM-DDTHH:MM:SSZ.
  static std::string utc_now() {
    const auto now      = std::chrono::system_clock::now();
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
      if (c == '"') {
        out += "\\\"";
      } else if (c == '\\') {
        out += "\\\\";
      } else {
        out += c;
      }
    }
    return out;
  }
};

}  // namespace oai::nef::app
