/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

#pragma once

#include <string>
#include <vector>

#include "config.hpp"

constexpr auto NEF_CONFIG_NAME_LABEL             = "NEF Config";
constexpr auto NEF_CONFIG_SUPPORT_FEATURES       = "support_features";
constexpr auto NEF_CONFIG_SUPPORT_FEATURES_LABEL = "Support Features";
constexpr auto NEF_CONFIG_AF_WHITELIST           = "af_whitelist";
constexpr auto NEF_CONFIG_AF_WHITELIST_LABEL     = "AF Whitelist";

// YAML sub-keys for a whitelist entry
constexpr auto NEF_CONFIG_AF_ID      = "af_id";
constexpr auto NEF_CONFIG_AF_API_KEY = "api_key";
constexpr auto NEF_CONFIG_AF_ALLOWED = "allowed_apis";

namespace oai::config::nef {

/**
 * One entry in the AF / SCS whitelist.
 *
 * YAML shape (sequence under nef.af_whitelist):
 *
 *   af_whitelist:
 *     - af_id: "my-af"
 *       api_key: "secret"          # optional; empty = no key check
 *       allowed_apis:              # optional; absent/empty = all APIs allowed
 *         - monitoring_event
 *         - traffic_influence
 */
struct af_whitelist_entry_t {
  std::string af_id;    ///< SCS/AS identifier
  std::string api_key;  ///< Pre-shared key (empty = unchecked)
  std::vector<std::string>
      allowed_apis;  ///< Allowed service names (empty = all)
};

class nef_config_type : public oai::config::nf {
  friend class nef_config;

 private:
  string_config_value m_support_features;
  // Structured whitelist – parsed from YAML sequence
  std::vector<af_whitelist_entry_t> m_af_whitelist;
  // JWT HMAC shared secret (empty = JWT validation disabled)
  std::string m_jwt_secret_key;

 public:
  explicit nef_config_type(
      const std::string& name, const std::string& host,
      const sbi_interface& sbi);

  void from_yaml(const YAML::Node& node) override;
  nlohmann::json to_json() override;
  bool from_json(const nlohmann::json& json_data) override;

  [[nodiscard]] std::string to_string(const std::string& indent) const override;
  void validate() override;

  [[nodiscard]] std::string get_support_features() const;
  void set_support_features(const std::string&);

  /**
   * Returns the parsed whitelist.
   * An empty vector means open-access (development/test mode).
   */
  [[nodiscard]] const std::vector<af_whitelist_entry_t>& get_af_whitelist()
      const;

  /**
   * Returns the configured JWT HMAC secret key.
   * An empty string means JWT validation is disabled (development mode).
   */
  [[nodiscard]] std::string get_jwt_secret_key() const;
};

}  // namespace oai::config::nef
