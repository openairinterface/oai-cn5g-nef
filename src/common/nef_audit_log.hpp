/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <string>

#include "logger.hpp"
#include "nef_audit_record.hpp"

namespace oai::nef::app {

class nef_audit {
 public:
  /// Emit one structured audit record to the NEF_APP log channel.
  static void log(
      const std::string& op, const std::string& resource,
      const std::string& af_id, const std::string& res_id, int http_code) {
    Logger::nef_app().info(
        "[AUDIT] %s",
        nef_audit_record::make_record(op, resource, af_id, res_id, http_code)
            .c_str());
  }
};

}  // namespace oai::nef::app
