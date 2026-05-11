/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_config_types.hpp"

#include <algorithm>
#include <sstream>

#include <rfl/json.hpp>

#include "logger.hpp"
#include "nef_config.hpp"

using namespace oai::config::nef;

// Internal typed structs for reflect-cpp serialisation — NOT exposed in header.
namespace {
struct AfWhitelistEntryData {
  rfl::Rename<"af_id", std::string> af_id;
  rfl::Rename<"api_key", std::optional<std::string>> api_key;
  rfl::Rename<"allowed_apis", std::optional<std::vector<std::string>>>
      allowed_apis;
};

struct NefConfigData {
  rfl::Rename<"Support Features", std::optional<std::string>> support_features;
  rfl::Rename<"AF Whitelist", std::optional<std::vector<AfWhitelistEntryData>>>
      af_whitelist;
};
}  // namespace

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

  NefConfigData data;
  data.support_features = m_support_features.get_value();

  std::vector<AfWhitelistEntryData> wl;
  wl.reserve(m_af_whitelist.size());
  for (const auto& entry : m_af_whitelist) {
    AfWhitelistEntryData e;
    e.af_id        = entry.af_id;
    e.api_key      = entry.api_key;
    e.allowed_apis = entry.allowed_apis;
    wl.push_back(std::move(e));
  }
  data.af_whitelist = std::move(wl);

  const auto nef_partial = nlohmann::json::parse(rfl::json::write(data));
  j[m_support_features.get_config_name()] = nef_partial["Support Features"];
  j[NEF_CONFIG_AF_WHITELIST_LABEL]        = nef_partial["AF Whitelist"];

  return j;
}

//------------------------------------------------------------------------------
bool nef_config_type::from_json(const nlohmann::json& json_data) {
  try {
    nf::from_json(json_data);

    const auto result = rfl::json::read<NefConfigData>(json_data.dump());
    if (!result) return false;
    const auto& d = *result;

    if (d.support_features.get()) {
      set_support_features(*d.support_features.get());
    }

    if (d.af_whitelist.get()) {
      m_af_whitelist.clear();
      for (const auto& e : *d.af_whitelist.get()) {
        const std::string& af_id_val = e.af_id.get();
        if (af_id_val.empty()) continue;
        af_whitelist_entry_t entry;
        entry.af_id   = af_id_val;
        entry.api_key = e.api_key.get().value_or("");
        entry.allowed_apis =
            e.allowed_apis.get().value_or(std::vector<std::string>{});
        m_af_whitelist.push_back(std::move(entry));
      }
    }

    return true;
  } catch (const std::exception&) {
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
          fmt::runtime(BASE_FORMATTER), OUTER_LIST_ELEM,
          m_support_features.get_config_name(), inner_width,
          m_support_features.get_value()));

  // Whitelist summary
  out.append(inner_indent)
      .append(fmt::format(
          fmt::runtime(BASE_FORMATTER), OUTER_LIST_ELEM,
          NEF_CONFIG_AF_WHITELIST_LABEL, inner_width,
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
