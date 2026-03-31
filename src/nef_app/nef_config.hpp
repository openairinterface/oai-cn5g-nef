/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include "config.hpp"
#include "nef_config_types.hpp"

// NEF config name used as YAML key and NF-list lookup key
const std::string NEF_CONFIG_NAME = "nef";

namespace oai::config::nef {

class nef_config : public oai::config::config {
 public:
  unsigned int instance = 0;

  explicit nef_config(
      const std::string& config_path, bool log_stdout, bool log_rot_file)
      : config(config_path, NEF_CONFIG_NAME, log_stdout, log_rot_file) {
    m_used_config_values = {
        LOG_LEVEL_CONFIG_NAME, NF_LIST_CONFIG_NAME, NF_CONFIG_HTTP_NAME,
        NEF_CONFIG_NAME};
    m_used_sbi_values = {NEF_CONFIG_NAME, NRF_CONFIG_NAME, AMF_CONFIG_NAME,
                         SMF_CONFIG_NAME, PCF_CONFIG_NAME, UDR_CONFIG_NAME};

    auto nef = std::make_shared<nef_config_type>(
        NEF_CONFIG_NAME, "oai-nef",
        sbi_interface("SBI", "oai-nef", 80, "v1", "eth0"));
    add_nf(NEF_CONFIG_NAME, nef);
  }

  std::shared_ptr<nef_config_type> nef() const {
    return std::static_pointer_cast<nef_config_type>(get_local());
  }
};

}  // namespace oai::config::nef
