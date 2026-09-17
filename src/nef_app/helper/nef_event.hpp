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

// Periodic task tick raised by task_manager, at 1 ms resolution. It is what
// drives the heartbeat timers.
using task_sig_t = oai::sba::task_sig_t;

// Fired when a NF (AMF/SMF/PCF) sends a notification to the NEF.
// Carries the internal NF subscription-id and the raw JSON payload.
typedef bs2::signal_type<
    void(const std::string& nf_sub_id, const nlohmann::json& notif),
    bs2::keywords::mutex_type<bs2::mutex>>::type nef_nf_notification_sig_t;

// Fired when a subscription's validity timer expires.
typedef bs2::signal_type<
    void(const std::string& sub_id),
    bs2::keywords::mutex_type<bs2::mutex>>::type nef_subscription_expired_sig_t;

class nef_app;

// The NEF event bus. It derives from oai::sba::nf_event, so the common
// signals — the task tick above among them — come with it; the two signals
// declared below are the NEF-specific additions.
//
// Note the get_instance() singleton below has no caller: main.cpp constructs
// the instance and hands it to nef_app and task_manager.
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

  // Subscribe to NF notifications received from the 5GC.
  bs2::connection subscribe_nf_notification(
      const nef_nf_notification_sig_t::slot_type& slot);

  // Subscribe to subscription-validity expiry.
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
