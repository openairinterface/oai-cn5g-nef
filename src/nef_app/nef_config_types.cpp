/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_config_types.hpp"

#include "logger.hpp"
#include "nef_config.hpp"

using namespace oai::config::nef;

//------------------------------------------------------------------------------
nef_config_type::nef_config_type(
    const std::string& name, const std::string& host, const sbi_interface& sbi)
    : nf(name, host, sbi) {
  m_config_name      = NEF_CONFIG_NAME_LABEL;
  m_support_features = string_config_value(
      NEF_CONFIG_SUPPORT_FEATURES_LABEL,
      "nnef-eventexposure,nnef-pfdmanagement");
  // m_af_whitelist is default-constructed as empty vector (open-access mode)
}

//------------------------------------------------------------------------------
void nef_config_type::from_yaml(const YAML::Node& node) {
  nf::from_yaml(node);

  for (const auto& elem : node) {
    auto key = elem.first.as<std::string>();

    if (key == NEF_CONFIG_SUPPORT_FEATURES) {
      m_support_features.from_yaml(elem.second);
    }

    if (key == NEF_CONFIG_AF_WHITELIST) {
      m_af_whitelist.clear();
      const YAML::Node& wl_node = elem.second;

      // Accept either null/scalar (empty = open-access) or a sequence
      if (!wl_node.IsSequence()) continue;

      for (const auto& entry_node : wl_node) {
        af_whitelist_entry_t entry;

        if (entry_node[NEF_CONFIG_AF_ID]) {
          entry.af_id = entry_node[NEF_CONFIG_AF_ID].as<std::string>();
        }
        if (entry_node[NEF_CONFIG_AF_API_KEY]) {
          entry.api_key = entry_node[NEF_CONFIG_AF_API_KEY].as<std::string>();
        }
        if (entry_node[NEF_CONFIG_AF_ALLOWED] &&
            entry_node[NEF_CONFIG_AF_ALLOWED].IsSequence()) {
          for (const auto& api_node : entry_node[NEF_CONFIG_AF_ALLOWED]) {
            entry.allowed_apis.push_back(api_node.as<std::string>());
          }
        }

        if (!entry.af_id.empty()) {
          m_af_whitelist.push_back(std::move(entry));
        }
      }
    }

    if (key == "security") {
      const YAML::Node& sec_node = elem.second;
      if (sec_node["jwt_secret"]) {
        m_jwt_secret_key = sec_node["jwt_secret"].as<std::string>();
      }
      if (sec_node["insecure_dev_mode"]) {
        m_insecure_dev_mode = sec_node["insecure_dev_mode"].as<bool>(false);
      }
    }
  }
}

//------------------------------------------------------------------------------
nlohmann::json nef_config_type::to_json() {
  nlohmann::json j = nf::to_json();

  j[m_support_features.get_config_name()] = m_support_features.to_json();

  nlohmann::json wl_arr = nlohmann::json::array();
  for (const auto& entry : m_af_whitelist) {
    nlohmann::json e;
    e[NEF_CONFIG_AF_ID]      = entry.af_id;
    e[NEF_CONFIG_AF_API_KEY] = entry.api_key;
    e[NEF_CONFIG_AF_ALLOWED] = entry.allowed_apis;
    wl_arr.push_back(e);
  }
  j[NEF_CONFIG_AF_WHITELIST_LABEL] = wl_arr;

  return j;
}

//------------------------------------------------------------------------------
bool nef_config_type::from_json(const nlohmann::json& json_data) {
  try {
    nf::from_json(json_data);

    if (json_data.contains(m_support_features.get_config_name())) {
      m_support_features.from_json(
          json_data[m_support_features.get_config_name()]);
    }

    if (json_data.contains(NEF_CONFIG_AF_WHITELIST_LABEL) &&
        json_data[NEF_CONFIG_AF_WHITELIST_LABEL].is_array()) {
      m_af_whitelist.clear();
      for (const auto& e : json_data[NEF_CONFIG_AF_WHITELIST_LABEL]) {
        af_whitelist_entry_t entry;
        if (e.contains(NEF_CONFIG_AF_ID))
          entry.af_id = e[NEF_CONFIG_AF_ID].get<std::string>();
        if (e.contains(NEF_CONFIG_AF_API_KEY))
          entry.api_key = e[NEF_CONFIG_AF_API_KEY].get<std::string>();
        if (e.contains(NEF_CONFIG_AF_ALLOWED) &&
            e[NEF_CONFIG_AF_ALLOWED].is_array()) {
          for (const auto& api : e[NEF_CONFIG_AF_ALLOWED])
            entry.allowed_apis.push_back(api.get<std::string>());
        }
        if (!entry.af_id.empty()) m_af_whitelist.push_back(std::move(entry));
      }
    }
    return true;
  } catch (nlohmann::detail::exception&) {
  } catch (std::exception&) {
  }
  return false;
}

//------------------------------------------------------------------------------
std::string nef_config_type::to_string(const std::string& indent) const {
  std::string out          = {};
  std::string inner_indent = indent + indent;
  unsigned int inner_width = get_inner_width(inner_indent.length());
  out.append(nf::to_string(indent));

  out.append(inner_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM, m_support_features.get_config_name(),
          inner_width, m_support_features.get_value()));

  // Whitelist summary
  out.append(inner_indent)
      .append(fmt::format(
          BASE_FORMATTER, OUTER_LIST_ELEM, NEF_CONFIG_AF_WHITELIST_LABEL,
          inner_width,
          m_af_whitelist.empty() ?
              std::string("(open-access / dev mode)") :
              std::to_string(m_af_whitelist.size()) + " entry/entries"));

  return out;
}

//------------------------------------------------------------------------------
void nef_config_type::validate() {
  nf::validate();
  if (m_af_whitelist.empty() && m_jwt_secret_key.empty()) {
    if (m_insecure_dev_mode) {
      Logger::system().warn(
          "NEF: insecure_dev_mode=true with no JWT secret and no AF whitelist "
          "– "
          "all requests will be permitted (development mode only)");
    } else {
      Logger::system().warn(
          "NEF: no JWT secret and no AF whitelist configured – "
          "fail-closed mode: all requests will be DENIED. "
          "Set insecure_dev_mode: true to allow unauthenticated access.");
    }
  }
}

//------------------------------------------------------------------------------
std::string nef_config_type::get_support_features() const {
  return m_support_features.get_value();
}

//------------------------------------------------------------------------------
void nef_config_type::set_support_features(const std::string& val) {
  m_support_features =
      string_config_value(NEF_CONFIG_SUPPORT_FEATURES_LABEL, val);
}

//------------------------------------------------------------------------------
const std::vector<af_whitelist_entry_t>& nef_config_type::get_af_whitelist()
    const {
  return m_af_whitelist;
}

//------------------------------------------------------------------------------
std::string nef_config_type::get_jwt_secret_key() const {
  return m_jwt_secret_key;
}
