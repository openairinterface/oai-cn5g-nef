/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_EVENT_HPP_SEEN
#define FILE_NEF_EVENT_HPP_SEEN

#include <boost/signals2.hpp>
#include <nlohmann/json.hpp>
#include <string>

#include "task_manager.hpp"
#include "nf_event.hpp"

namespace bs2 = boost::signals2;

namespace oai {
namespace nef {
namespace app {

// Periodic task tick (1ms resolution, drives heartbeat timers)
using task_sig_t = oai::sba::task_sig_t;

// Fired when a NF (AMF/SMF/PCF) sends a notification to NEF
// Carries the internal NF subscription-id and the raw JSON payload
typedef bs2::signal_type<
    void(const std::string& nf_sub_id, const nlohmann::json& notif),
    bs2::keywords::mutex_type<bs2::mutex>>::type nef_nf_notification_sig_t;

// Fired when a subscription validity timer expires
typedef bs2::signal_type<
    void(const std::string& sub_id),
    bs2::keywords::mutex_type<bs2::mutex>>::type nef_subscription_expired_sig_t;

class nef_app;

class nef_event : public oai::sba::nf_event {
 public:
  nef_event() : oai::sba::nf_event(){};
  nef_event(nef_event const&) = delete;
  virtual ~nef_event();
  void operator=(nef_event const&) = delete;

  static nef_event& get_instance() {
    static nef_event instance;
    return instance;
  }

  // NF notification received from 5GC
  bs2::connection subscribe_nf_notification(
      const nef_nf_notification_sig_t::slot_type& slot);

  // Subscription validity expired
  bs2::connection subscribe_subscription_expired(
      const nef_subscription_expired_sig_t::slot_type& slot);

 private:
  friend class nef_app;

  nef_nf_notification_sig_t nf_notification;
  nef_subscription_expired_sig_t subscription_expired;
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_EVENT_HPP_SEEN */
