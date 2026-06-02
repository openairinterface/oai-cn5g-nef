/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_EVENT_SIG_HPP_SEEN
#define FILE_NEF_EVENT_SIG_HPP_SEEN

#include <boost/signals2.hpp>
#include <rfl/Generic.hpp>
#include <string>

namespace bs2 = boost::signals2;

namespace oai {
namespace nef {
namespace app {

// Periodic task tick (1ms resolution, drives heartbeat timers)
typedef bs2::signal_type<
    void(uint64_t), bs2::keywords::mutex_type<bs2::mutex>>::type task_sig_t;

// Fired when a 5GC NF (AMF/SMF/PCF) sends a notification to NEF
// Carries the internal NF subscription-id and the raw JSON payload
typedef bs2::signal_type<
    void(const std::string& nf_sub_id, const rfl::Generic& notif),
    bs2::keywords::mutex_type<bs2::mutex>>::type nef_nf_notification_sig_t;

// Fired when a subscription validity timer expires
typedef bs2::signal_type<
    void(const std::string& sub_id),
    bs2::keywords::mutex_type<bs2::mutex>>::type nef_subscription_expired_sig_t;

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_EVENT_SIG_HPP_SEEN */
