/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

#include "nef_event.hpp"

#include <boost/signals2.hpp>

#include "nef_app.hpp"
#include "nef_event_sig.hpp"

using namespace oai::nef::app;
namespace bs2 = boost::signals2;

// Task tick 
bs2::connection nef_event::subscribe_task_tick(
    const task_sig_t::slot_type& sig, uint64_t period, uint64_t start) {
  auto f = [period, start, sig](uint64_t t) {
    if (t >= start && (t - start) % period == 0) sig(t);
  };
  return task_tick.connect(f);
}

bs2::connection nef_event::subscribe_task_tick_extended(
    const task_sig_t::extended_slot_type& sig, uint64_t period,
    uint64_t start) {
  auto f = [period, start, sig](const bs2::connection& c, uint64_t t) {
    if (t >= start && (t - start) % period == 0) sig(c, t);
  };
  return task_tick.connect_extended(f);
}

// NF notification received from 5GC
bs2::connection nef_event::subscribe_nf_notification(
    const nef_nf_notification_sig_t::slot_type& sig) {
  return nf_notification.connect(sig);
}

// Subscription validity expired
bs2::connection nef_event::subscribe_subscription_expired(
    const nef_subscription_expired_sig_t::slot_type& sig) {
  return subscription_expired.connect(sig);
}
