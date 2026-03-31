/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_SUBSCRIPTION_HPP_SEEN
#define FILE_NEF_SUBSCRIPTION_HPP_SEEN

#include <chrono>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/date_time/posix_time/time_parsers.hpp>
#include <boost/signals2.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "logger.hpp"
#include "nef.h"
#include "nef_event.hpp"

namespace bs2 = boost::signals2;

namespace oai {
namespace nef {
namespace app {

/**
 * Represents an AF subscription to one of the NEF Northbound services.
 *
 * Each record stores:
 *  - The external AF subscription-id (returned to the AF).
 *  - The internal NF subscription-id obtained from the southbound 5GC NF.
 *  - The AF notification URI.
 *  - The service type (monitoring, TI, PFD, BDT, QoS, analytics).
 *  - Validity time + expiry boost::signals2 connection.
 */
class nef_subscription {
 public:
  explicit nef_subscription(nef_event& ev);
  nef_subscription(nef_subscription const&) = delete;
  virtual ~nef_subscription();
  void operator=(nef_subscription const&) = delete;

  // ── AF Subscription ID ────────────────────────────────────────────────────
  void set_af_subscription_id(const std::string& sub_id);
  std::string get_af_subscription_id() const;

  // ── NF Subscription ID (southbound) ──────────────────────────────────────
  void set_nf_subscription_id(const std::string& nf_sub_id);
  std::string get_nf_subscription_id() const;

  // ── Notification URI (AF endpoint) ───────────────────────────────────────
  void set_notification_uri(const std::string& uri);
  std::string get_notification_uri() const;

  // ── NEF service type ──────────────────────────────────────────────────────
  void set_service_type(nef_service_type_t svc);
  nef_service_type_t get_service_type() const;

  // ── Target NF type ───────────────────────────────────────────────────────
  void set_target_nf_type(nf_type_t nf_type);
  nf_type_t get_target_nf_type() const;

  // ── Validity time ─────────────────────────────────────────────────────────
  void set_validity_time(const boost::posix_time::ptime& t);
  boost::posix_time::ptime get_validity_time() const;

  // ── Absolute expiry time (monitorExpireTime) ─────────────────────────────
  void set_expire_time(const std::chrono::system_clock::time_point& t);
  std::chrono::system_clock::time_point get_expire_time() const;
  bool has_expire_time() const;

  // ── HTTP version used by AF ───────────────────────────────────────────────
  void set_http_version(uint8_t ver);
  uint8_t get_http_version() const;

  // ── SCS/AS identifier (external app-id from 3GPP) ─────────────────────────
  void set_scs_as_id(const std::string& id);
  std::string get_scs_as_id() const;

  // ── Subscription body (cached for retrieval / notifications) ──────────────
  void set_subscription_data(const nlohmann::json& data);
  nlohmann::json get_subscription_data() const;

  void display() const;

 private:
  std::string            m_af_sub_id;
  std::string            m_nf_sub_id;        // obtained from AMF/SMF/PCF/UDR
  std::string            m_notification_uri;
  std::string            m_scs_as_id;
  nef_service_type_t     m_service_type      = nef_service_type_t::NEF_SERVICE_TYPE_MONITORING_EVENT;
  nf_type_t              m_target_nf_type    = nf_type_t::NF_TYPE_AMF;
  boost::posix_time::ptime m_validity_time;
  std::chrono::system_clock::time_point m_expire_time{};
  bool                   m_has_expire_time   = false;
  uint8_t                m_http_version      = 1;
  nlohmann::json         m_subscription_data = {};

  nef_event&             m_event_sub;
  bs2::connection        m_ev_connection;
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_SUBSCRIPTION_HPP_SEEN */
