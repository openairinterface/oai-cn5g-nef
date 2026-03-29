/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

#ifndef FILE_NEF_EVENT_SIG_HPP_SEEN
#define FILE_NEF_EVENT_SIG_HPP_SEEN

#include <boost/signals2.hpp>
#include <nlohmann/json.hpp>
#include <string>

namespace bs2 = boost::signals2;

namespace oai {
namespace nef {
namespace app {

// Periodic task tick (1ms resolution, drives heartbeat timers)
typedef bs2::signal_type<
    void(uint64_t),
    bs2::keywords::mutex_type<bs2::mutex>>::type task_sig_t;

// Fired when a 5GC NF (AMF/SMF/PCF) sends a notification to NEF
// Carries the internal NF subscription-id and the raw JSON payload
typedef bs2::signal_type<
    void(const std::string& nf_sub_id, const nlohmann::json& notif),
    bs2::keywords::mutex_type<bs2::mutex>>::type nef_nf_notification_sig_t;

// Fired when a subscription validity timer expires
typedef bs2::signal_type<
    void(const std::string& sub_id),
    bs2::keywords::mutex_type<bs2::mutex>>::type nef_subscription_expired_sig_t;

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_EVENT_SIG_HPP_SEEN */
