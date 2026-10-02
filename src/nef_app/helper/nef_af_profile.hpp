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

#include "nef_event.hpp"

namespace oai {
namespace nef {
namespace app {

/**
 * One registered AF (SCS/AS).
 *
 * A profile lives exactly as long as the AF has subscriptions: nef_app creates
 * it on the first successful subscription and destroys it when the last one is
 * deleted. All accessors are guarded, so profiles are safe to share across the
 * HTTP worker threads.
 */
class nef_af_profile : public std::enable_shared_from_this<nef_af_profile> {
 public:
  explicit nef_af_profile(const std::shared_ptr<nef_event>& ev);
  nef_af_profile(nef_af_profile const&) = delete;
  void operator=(nef_af_profile const&) = delete;
  virtual ~nef_af_profile();

  // AF identity
  void set_af_id(const std::string& af_id);
  std::string get_af_id() const;

  // Pre-shared API key. Optional: with an empty stored key no key is
  // enforced, so validate_api_key() always passes.
  void set_api_key(const std::string& key);
  bool validate_api_key(const std::string& provided_key) const;

  // Service names this AF may use. An empty list permits everything.
  void set_allowed_apis(const std::vector<std::string>& apis);
  bool is_api_allowed(const std::string& api_name) const;

  // Active subscription tracking.
  //
  // remove_subscription_id() returns false when the id was not there.
  // nef_app watches has_no_subscriptions() to know when to drop the profile.
  void add_subscription_id(const std::string& sub_id);
  bool remove_subscription_id(const std::string& sub_id);
  std::vector<std::string> get_subscription_ids() const;
  bool has_no_subscriptions() const;

  nlohmann::json to_json() const;

  void display() const;

 private:
  std::shared_ptr<nef_event> m_event_sub;
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
