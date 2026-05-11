/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_af_profile.hpp"

#include <algorithm>
#include <rfl/json.hpp>

#include "logger.hpp"

using namespace oai::nef::app;

//------------------------------------------------------------------------------
nef_af_profile::nef_af_profile(nef_event& ev) : m_event_sub(ev) {
  Logger::nef_app().debug("Creating NEF AF profile instance");
}

//------------------------------------------------------------------------------
nef_af_profile::~nef_af_profile() {
  Logger::nef_app().debug("Destroying NEF AF profile: %s", m_af_id.c_str());
}

//------------------------------------------------------------------------------
void nef_af_profile::set_af_id(const std::string& af_id) {
  std::unique_lock lock(m_mutex);
  m_af_id = af_id;
}

//------------------------------------------------------------------------------
std::string nef_af_profile::get_af_id() const {
  std::shared_lock lock(m_mutex);
  return m_af_id;
}

//------------------------------------------------------------------------------
void nef_af_profile::set_api_key(const std::string& key) {
  std::unique_lock lock(m_mutex);
  m_api_key = key;
}

//------------------------------------------------------------------------------
bool nef_af_profile::validate_api_key(const std::string& provided_key) const {
  std::shared_lock lock(m_mutex);
  // Empty stored key → no key enforcement
  if (m_api_key.empty()) return true;
  return m_api_key == provided_key;
}

//------------------------------------------------------------------------------
void nef_af_profile::set_allowed_apis(const std::vector<std::string>& apis) {
  std::unique_lock lock(m_mutex);
  m_allowed_apis = apis;
}

//------------------------------------------------------------------------------
bool nef_af_profile::is_api_allowed(const std::string& api_name) const {
  std::shared_lock lock(m_mutex);
  // Empty list → all APIs allowed
  if (m_allowed_apis.empty()) return true;
  return std::find(m_allowed_apis.begin(), m_allowed_apis.end(), api_name) !=
         m_allowed_apis.end();
}

//------------------------------------------------------------------------------
void nef_af_profile::add_subscription_id(const std::string& sub_id) {
  std::unique_lock lock(m_mutex);
  // Avoid duplicates
  if (std::find(m_subscription_ids.begin(), m_subscription_ids.end(), sub_id) ==
      m_subscription_ids.end()) {
    m_subscription_ids.push_back(sub_id);
    Logger::nef_app().debug(
        "AF %s: added subscription %s (total: %zu)", m_af_id.c_str(),
        sub_id.c_str(), m_subscription_ids.size());
  }
}

//------------------------------------------------------------------------------
bool nef_af_profile::remove_subscription_id(const std::string& sub_id) {
  std::unique_lock lock(m_mutex);
  auto it =
      std::find(m_subscription_ids.begin(), m_subscription_ids.end(), sub_id);
  if (it == m_subscription_ids.end()) return false;
  m_subscription_ids.erase(it);
  Logger::nef_app().debug(
      "AF %s: removed subscription %s (remaining: %zu)", m_af_id.c_str(),
      sub_id.c_str(), m_subscription_ids.size());
  return true;
}

//------------------------------------------------------------------------------
std::vector<std::string> nef_af_profile::get_subscription_ids() const {
  std::shared_lock lock(m_mutex);
  return m_subscription_ids;
}

//------------------------------------------------------------------------------
bool nef_af_profile::has_no_subscriptions() const {
  std::shared_lock lock(m_mutex);
  return m_subscription_ids.empty();
}

//------------------------------------------------------------------------------
// Internal typed struct — intentionally not exposed in the header.
namespace {
struct AfProfileData {
  rfl::Rename<"afId", std::string> af_id;
  rfl::Rename<"allowedApis", std::vector<std::string>> allowed_apis;
  rfl::Rename<"subscriptionIds", std::vector<std::string>> subscription_ids;
};
}  // namespace

//------------------------------------------------------------------------------
std::string nef_af_profile::to_json_str() const {
  std::shared_lock lock(m_mutex);
  AfProfileData data;
  data.af_id            = m_af_id;
  data.allowed_apis     = m_allowed_apis;
  data.subscription_ids = m_subscription_ids;
  // api_key intentionally omitted from JSON representation
  return rfl::json::write(data);
}

//------------------------------------------------------------------------------
void nef_af_profile::display() const {
  Logger::nef_app().debug(
      "NEF AF Profile: af_id=%s, allowed_apis=%zu, active_subs=%zu",
      m_af_id.c_str(), m_allowed_apis.size(), m_subscription_ids.size());
}
