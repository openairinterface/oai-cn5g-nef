/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"

constexpr auto NEF_CONFIG_NAME_LABEL             = "NEF Config";
constexpr auto NEF_CONFIG_SUPPORT_FEATURES       = "support_features";
constexpr auto NEF_CONFIG_SUPPORT_FEATURES_LABEL = "Support Features";
constexpr auto NEF_CONFIG_AF_WHITELIST           = "af_whitelist";
constexpr auto NEF_CONFIG_AF_WHITELIST_LABEL     = "AF Whitelist";
constexpr auto NEF_CONFIG_DISPATCHER_POOL_SIZE   = "dispatcher_pool_size";

// YAML sub-keys of one whitelist entry
constexpr auto NEF_CONFIG_AF_ID      = "af_id";
constexpr auto NEF_CONFIG_AF_API_KEY = "api_key";
constexpr auto NEF_CONFIG_AF_ALLOWED = "allowed_apis";

namespace oai::config::nef {

/**
 * One entry in the AF / SCS whitelist.
 *
 * YAML shape, a sequence under nef.af_whitelist:
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
  // Structured whitelist, parsed from the YAML sequence. Empty = open access.
  std::vector<af_whitelist_entry_t> m_af_whitelist;
  // JWT HMAC shared secret. Empty disables JWT validation.
  std::string m_jwt_secret_key;
  // Fail-open switch. When true, requests are allowed even with no JWT secret
  // and no whitelist configured.
  // Default false, i.e. fail-closed: such a deployment denies everything.
  bool m_insecure_dev_mode{false};
  // Async dispatcher thread-pool size. 0 means auto: the server keeps its own
  // default of http_workers + 2.
  // Shrink with care. Until an in-flight request cap exists, the pool size is
  // the only thing limiting concurrency.
  uint32_t m_dispatcher_pool_size{0};

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

  // An empty whitelist means open access (development / test).
  [[nodiscard]] const std::vector<af_whitelist_entry_t>& get_af_whitelist()
      const;

  // An empty secret disables JWT validation (development mode).
  [[nodiscard]] std::string get_jwt_secret_key() const;

  [[nodiscard]] bool get_insecure_dev_mode() const {
    return m_insecure_dev_mode;
  }

  [[nodiscard]] uint32_t get_dispatcher_pool_size() const {
    return m_dispatcher_pool_size;
  }
};

}  // namespace oai::config::nef
