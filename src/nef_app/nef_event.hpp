/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_EVENT_HPP_SEEN
#define FILE_NEF_EVENT_HPP_SEEN

#include <boost/signals2.hpp>
#include <string>

#include "nef_event_sig.hpp"

namespace oai {
namespace nef {
namespace app {

class nef_app;
class task_manager;

class nef_event {
 public:
  nef_event()                           = default;
  nef_event(nef_event const&)           = delete;
  void operator=(nef_event const&)      = delete;

  static nef_event& get_instance() {
    static nef_event instance;
    return instance;
  }

  // Task tick
  bs2::connection subscribe_task_tick(const task_sig_t::slot_type& slot,
                                      uint64_t period, uint64_t start = 0);

  bs2::connection subscribe_task_tick_extended(
      const task_sig_t::extended_slot_type& slot,
      uint64_t period, uint64_t start = 0);

  // NF notification received from 5GC
  bs2::connection subscribe_nf_notification(
      const nef_nf_notification_sig_t::slot_type& slot);

  // Subscription validity expired
  bs2::connection subscribe_subscription_expired(
      const nef_subscription_expired_sig_t::slot_type& slot);

 private:
  friend class nef_app;
  friend class task_manager;

  task_sig_t                      task_tick;
  nef_nf_notification_sig_t       nf_notification;
  nef_subscription_expired_sig_t  subscription_expired;
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_EVENT_HPP_SEEN */
