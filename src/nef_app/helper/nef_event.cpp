/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_event.hpp"

#include <boost/signals2.hpp>

using namespace oai::nef::app;
namespace bs2 = boost::signals2;

//------------------------------------------------------------------------------
nef_event::~nef_event() {}

// NF notification received from the 5GC
//------------------------------------------------------------------------------
bs2::connection nef_event::subscribe_nf_notification(
    const nef_nf_notification_sig_t::slot_type& sig) {
  return nf_notification.connect(sig);
}

// Subscription validity timer expired
//------------------------------------------------------------------------------
bs2::connection nef_event::subscribe_subscription_expired(
    const nef_subscription_expired_sig_t::slot_type& sig) {
  return subscription_expired.connect(sig);
}
