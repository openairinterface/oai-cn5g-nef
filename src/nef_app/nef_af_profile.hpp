/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_AF_PROFILE_HPP_SEEN
#define FILE_NEF_AF_PROFILE_HPP_SEEN

#include <memory>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>
#include <vector>

#include "logger.hpp"
#include "nef_event.hpp"

namespace oai {
namespace nef {
namespace app {

/**
 * Tracks a single registered AF (SCS/AS) inside NEF.
 *
 * NEF tracks each AF that has at least one active subscription.
 *
 * Lifecycle:
 *   - Created by nef_app on the AF's first successful subscription.
 *   - Updated as subscriptions are added / removed.
 *   - Destroyed when the AF's last subscription is deleted.
 */
class nef_af_profile : public std::enable_shared_from_this<nef_af_profile> {
 public:
  explicit nef_af_profile(nef_event& ev);
  nef_af_profile(nef_af_profile const&) = delete;
  void operator=(nef_af_profile const&) = delete;
  virtual ~nef_af_profile();

  // AF identity
  /*
   * Set the AF / SCS identifier.
   * @param [const std::string&] af_id: SCS/AS identifier
   */
  void set_af_id(const std::string& af_id);

  /*
   * Get the AF / SCS identifier.
   * @return std::string
   */
  std::string get_af_id() const;

  // API key (pre-shared, optional)
  /*
   * Set the pre-shared API key for this AF.
   * @param [const std::string&] key
   */
  void set_api_key(const std::string& key);

  /*
   * Validate the supplied key against the stored one.
   * Returns true if stored key is empty (no key enforced) or keys match.
   * @param [const std::string&] provided_key
   * @return bool
   */
  bool validate_api_key(const std::string& provided_key) const;

  // Allowed APIs
  /*
   * Set the list of service names this AF is allowed to use.
   * An empty list means all services are permitted.
   * @param [const std::vector<std::string>&] apis
   */
  void set_allowed_apis(const std::vector<std::string>& apis);

  /*
   * Check whether a specific API name is permitted for this AF.
   * @param [const std::string&] api_name
   * @return bool
   */
  bool is_api_allowed(const std::string& api_name) const;

  // Active subscription tracking
  /*
   * Record a new subscription as belonging to this AF.
   * @param [const std::string&] sub_id: AF subscription ID
   */
  void add_subscription_id(const std::string& sub_id);

  /*
   * Remove a subscription from this AF's list.
   * @param [const std::string&] sub_id: AF subscription ID
   * @return true if the sub was found and removed
   */
  bool remove_subscription_id(const std::string& sub_id);

  /*
   * Return a snapshot of all active subscription IDs for this AF.
   * @return std::vector<std::string>
   */
  std::vector<std::string> get_subscription_ids() const;

  /*
   * Returns true if this AF has no active subscriptions.
   * Used by nef_app to decide when to destroy the profile.
   */
  bool has_no_subscriptions() const;

  // Serialization
  nlohmann::json to_json() const;

  void display() const;

 private:
  nef_event& m_event_sub;
  std::string m_af_id;
  std::string m_api_key;                    // empty = unchecked
  std::vector<std::string> m_allowed_apis;  // empty = all allowed
  std::vector<std::string> m_subscription_ids;

  mutable std::shared_mutex m_mutex;
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_AF_PROFILE_HPP_SEEN */
