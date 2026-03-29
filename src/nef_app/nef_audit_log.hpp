/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1 (the "License"); you may not use this
 * file except in compliance with the License.
 */

/**
 * @file nef_audit_log.hpp
 * @brief Structured JSON audit logger for NEF resource mutations.
 *
 * All CREATE / UPDATE / DELETE / PATCH operations on NEF-managed resources
 * produce a single-line JSON audit record at INFO level in the NEF_APP
 * logging channel, prefixed with "[AUDIT]" for easy grepping.
 *
 * Usage:
 *   nef_audit::log("CREATE", "TI", af_id, app_session_id, 201);
 *
 * The JSON builder lives in nef_audit_record.hpp (no logger dependency) so
 * it can be unit-tested without linking spdlog.
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
      const std::string& op,
      const std::string& resource,
      const std::string& af_id,
      const std::string& res_id,
      int                http_code) {
    Logger::nef_app().info(
        "[AUDIT] %s",
        nef_audit_record::make_record(op, resource, af_id, res_id, http_code)
            .c_str());
  }
};

}  // namespace oai::nef::app
