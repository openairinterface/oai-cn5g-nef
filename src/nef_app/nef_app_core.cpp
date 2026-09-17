/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_app.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/bind/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <cctype>
#include <chrono>
#include <ctime>
#include <sstream>

#include "3gpp_29.500.h"
#include "logger.hpp"
#include "nef_audit_log.hpp"
#include "nef_callback_uri_validator.hpp"
#include "nef_input_validation.hpp"
#include "nef_pfd_atomicity.hpp"
#include "nef_pfd_management_quality.hpp"
#include "nef_client.hpp"
#include "nef_config.hpp"
#include "nef_jwt.hpp"
#include "nef_notification_mapper.hpp"
#include "nef_async_parse_helpers.hpp"
#include "nef_sbi_helper.hpp"
#include "nef_sbi_response_policy.hpp"

#include "AmfCreatedEventSubscription.h"
#include "AsSessionWithQoSSubscription.h"
#include "UserPlaneEvent.h"
#include "BdtPolicy.h"
#include "Helpers.h"
#include "NefEvent_anyOf.h"
#include "NefEventExposureSubsc.h"
#include "TrafficInfluData.h"
#include "TrafficInfluDataPatch.h"

#include <algorithm>
#include "nef_app_internal.hpp"

using namespace oai::nef::app;
using namespace boost::placeholders;
using namespace oai::common::sbi;

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;
extern std::shared_ptr<oai::sba::http_client> http_client_inst;

// Per-request bearer token.
//
// The storage is thread_local, so it is implicitly scoped to whichever
// thread is running the request right now. That is exactly the trap: a
// request does NOT stay on one thread. A phase-1 method runs on a dispatcher
// worker and then hands off to the oai-http-io pool mid-request, and the
// token does not travel with it.
//
// Sync path: safe and zero-maintenance. nef_app_adapter::execute_with_token
// wraps every execute_* in a bearer_token_scope whose destructor clears, so
// it is exception-safe by construction.
//
// Async path: manual, and the ORDERING is the invariant -- not the mere
// presence of a clear. The 26 phase-1 methods set the token once and then
// call clear_request_bearer_token() by hand on every exit path (133 call
// sites). Each clear must happen BEFORE the southbound fire and BEFORE every
// sink(...), not merely somewhere in the function. Otherwise a token
// outlives its request on a worker thread that is then reused by an
// unrelated one.
//
// That is why the obvious cleanup does not work: a naive "just wrap it in a
// scope guard" moves the clear after sink() and breaks precisely this
// ordering.
//
// No cont_* sets or clears the token, by design. The async split banner in
// nef_app_internal.hpp describes how the two halves divide the work.
//
// Open question for the maintainer: continuations mutate stores and do
// southbound cleanup with no token and therefore no re-authorization.
// Authorization happens once, in phase 1. One-shot by design, or a gap?
//
// get_request_bearer_token() currently has no callers.
namespace {
thread_local std::string g_request_bearer_token;
}

//------------------------------------------------------------------------------
bool nef_app::is_valid_app_session_id(const std::string& id) {
  if (id.empty() || id.size() > 253) return false;
  if (id.find("..") != std::string::npos) return false;
  for (const unsigned char c : id) {
    if (c == '/' || c == '\\') return false;
    if (std::isspace(c) || std::iscntrl(c)) return false;
  }
  return true;
}

//------------------------------------------------------------------------------
// Constructor / Destructor
nef_app::nef_app(const std::string& config_file, std::shared_ptr<nef_event>& ev)
    : m_event_sub(ev) {
  Logger::nef_app().startup("Starting NEF application...");

  generate_uuid();

  // Security configuration check — fail-closed by default
  {
    auto nef_cfg               = nef_config_inst->nef();
    const bool whitelist_empty = nef_cfg->get_af_whitelist().empty();
    const bool jwt_empty       = nef_cfg->get_jwt_secret_key().empty();
    if (whitelist_empty && jwt_empty) {
      if (nef_cfg->get_insecure_dev_mode()) {
        Logger::nef_app().warn(
            "\n"
            "╔══════════════════════════════════════════════════════════════╗\n"
            "║  SECURITY WARNING: insecure_dev_mode IS ENABLED              ║\n"
            "║  No authentication configured (no JWT secret, no whitelist). ║\n"
            "║  ALL requests will be permitted without any auth check.      ║\n"
            "║  DO NOT USE THIS CONFIGURATION IN PRODUCTION!                ║\n"
            "╚══════════════════════════════════════════════════════════════╝");
      } else {
        Logger::nef_app().info(
            "Auth not configured (no JWT secret, no AF whitelist). "
            "Fail-closed mode: all requests will be DENIED. "
            "Set insecure_dev_mode: true to allow unauthenticated access "
            "(e.g., for testing).");
      }
    }
  }

  // nef_client is an oai::sba::nf_service: it takes the event subscriber and
  // the SBI client the base retains. The heartbeat below stays here rather
  // than being handed to the base's timer, because NEF advertises a 50 s
  // heartBeatTimer and the base defaults to 10 s.
  m_nef_client = std::make_shared<nef_client>(ev, http_client_inst);

  // Bounded thread pool for notification forwarding (4 workers, 1000-task
  // queue).
  // TODO: expose num_threads / max_queue as nef_config parameters.
  m_notification_pool = std::make_unique<notification_thread_pool>(4, 1000);

  subscribe_nf_notification();

  uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

  // Register to NRF
  if (m_nef_client->register_to_nrf()) {
    Logger::nef_app().info("NEF registered to NRF");
    // Periodic NRF heartbeat every 50 s (50000 ms ticks)
    // TODO: get heartbeat interval from NRF response
    constexpr uint64_t HEARTBEAT_MS = 50000;
    auto hb_conn                    = m_event_sub->subscribe_task_nf_heartbeat(
        [this](uint64_t /*t*/) { m_nef_client->send_heartbeat_to_nrf(); },
        HEARTBEAT_MS, now_ms + HEARTBEAT_MS);
    m_connections.push_back(hb_conn);
  } else {
    Logger::nef_app().error("Failed to register NEF to NRF");
    // Exist?
  }

  constexpr uint64_t SUBSCRIPTION_EXPIRY_CHECK_MS = 1000;
  auto expiry_conn = m_event_sub->subscribe_task_nf_heartbeat(
      [this](uint64_t t) { handle_subscription_expiry_tick(t); },
      SUBSCRIPTION_EXPIRY_CHECK_MS, now_ms + SUBSCRIPTION_EXPIRY_CHECK_MS);
  m_connections.push_back(expiry_conn);

  Logger::nef_app().startup("NEF application started");
}

//------------------------------------------------------------------------------
nef_app::~nef_app() {
  Logger::nef_app().debug("Destroying NEF application...");
  for (auto& c : m_connections) {
    if (c.connected()) c.disconnect();
  }
  if (m_notification_pool) {
    m_notification_pool->stop();  // drain queue and join workers
  }
  // Only deregister if not already done explicitly (e.g. by graceful shutdown).
  if (!m_deregistered.exchange(true, std::memory_order_acq_rel)) {
    m_nef_client->deregister_from_nrf();
  }
}

//------------------------------------------------------------------------------
void nef_app::deregister_from_nrf() {
  if (m_deregistered.exchange(true, std::memory_order_acq_rel))
    return;  // double-deregistration guard
  m_nef_client->deregister_from_nrf();
}

// Utility functions
//------------------------------------------------------------------------------
void nef_app::generate_uuid() {
  m_nef_instance_id =
      boost::uuids::to_string(boost::uuids::random_generator()());
  Logger::nef_app().info("NEF instance ID: %s", m_nef_instance_id.c_str());
}

//------------------------------------------------------------------------------
void nef_app::generate_af_subscription_id(std::string& sub_id) {
  uint32_t id = m_sub_id_generator.get_uid();
  std::ostringstream oss;
  oss << std::hex << id;
  sub_id = oss.str();
}

// Authorization
//------------------------------------------------------------------------------
bool nef_app::authorize_af_request(
    const std::string& scs_as_id, const std::string& api_name) const {
  auto nef_cfg          = nef_config_inst->nef();
  const auto& wl        = nef_cfg->get_af_whitelist();
  const auto jwt_secret = nef_cfg->get_jwt_secret_key();

  if (!g_request_bearer_token.empty()) {
    static const nef_jwt jwt_validator;
    if (!jwt_validator.validate_af_token(
            g_request_bearer_token, api_name, scs_as_id)) {
      Logger::nef_app().warn(
          "JWT validation failed for AF %s on API %s", scs_as_id.c_str(),
          api_name.c_str());
      return false;
    }
    // JWT valid: allow immediately — do not fall through to whitelist check
    return true;
  } else if (!jwt_secret.empty()) {
    Logger::nef_app().warn(
        "Missing bearer token for AF %s while jwt_secret is configured",
        scs_as_id.c_str());
    return false;
  }

  // Security configuration check — fail-closed by default
  if (wl.empty() && jwt_secret.empty()) {
    if (nef_cfg->get_insecure_dev_mode()) {
      Logger::nef_app().debug(
          "insecure_dev_mode: allowing %s on %s (no auth configured)",
          scs_as_id.c_str(), api_name.c_str());
      return true;
    } else {
      Logger::nef_app().warn(
          "authorize_af_request(): auth not configured (no JWT secret, no AF "
          "whitelist) – denying %s on %s. Set insecure_dev_mode: true to "
          "allow unauthenticated access (e.g., for testing).",
          scs_as_id.c_str(), api_name.c_str());
      return false;
    }
  }

  // Whitelist configured but empty of matching entries — deny.
  if (wl.empty()) {
    Logger::nef_app().warn(
        "AF %s denied: whitelist is configured but empty", scs_as_id.c_str());
    return false;
  }

  for (const auto& entry : wl) {
    if (entry.af_id != scs_as_id) continue;

    // AF found in whitelist – check allowed APIs
    if (entry.allowed_apis.empty()) {
      Logger::nef_app().debug("AF %s allowed on all APIs", scs_as_id.c_str());
      return true;
    }
    bool allowed = std::find(
                       entry.allowed_apis.begin(), entry.allowed_apis.end(),
                       api_name) != entry.allowed_apis.end();
    if (!allowed) {
      Logger::nef_app().warn(
          "AF %s is NOT allowed to access API %s", scs_as_id.c_str(),
          api_name.c_str());
    }
    return allowed;
  }

  // AF not in whitelist at all
  Logger::nef_app().warn(
      "AF %s is not in the whitelist – denying request", scs_as_id.c_str());
  return false;
}

//------------------------------------------------------------------------------
bool nef_app::authorize_nnef_request(const std::string& api_name) const {
  // For Nnef (SBI) requests the consumer NF identity is in the bearer-token
  // 'sub' claim rather than the URL path.  Extract it and delegate to the
  // standard authorize_af_request() which handles JWT verification, whitelist
  // look-up, and insecure_dev_mode consistently.
  std::string nf_sub;
  if (!g_request_bearer_token.empty()) {
    static const nef_jwt jwt_validator;
    if (!jwt_validator.extract_sub_claim(g_request_bearer_token, nf_sub)) {
      Logger::nef_app().warn(
          "authorize_nnef_request(): malformed bearer token "
          "(cannot extract sub claim) for Nnef service '%s'",
          api_name.c_str());
      return false;
    }
  }
  return authorize_af_request(nf_sub, api_name);
}

// The four "not authorized" rejections. Each returns true when the caller
// must stop; false leaves response_body / the sink untouched.
//
// The response_sink overloads clear the bearer token BEFORE they answer,
// because that ordering is the invariant -- see the thread_local banner at
// the top of this file. It now holds in two places instead of seventeen.
//
// AF and NF are deliberately not merged: different predicate, different
// detail string, and merging them would change what the NF responses say.
//------------------------------------------------------------------------------
bool nef_app::reject_unauthorized_af(
    const std::string& af_id, const std::string& api_name,
    nlohmann::json& response_body, int& http_code) const {
  if (authorize_af_request(af_id, api_name)) return false;
  http_code     = http_status_code::FORBIDDEN;
  response_body = make_problem_detail(
      http_status_code::FORBIDDEN, "AF not authorized for this service");
  return true;
}

//------------------------------------------------------------------------------
bool nef_app::reject_unauthorized_af(
    const std::string& af_id, const std::string& api_name,
    const response_sink& sink) const {
  if (authorize_af_request(af_id, api_name)) return false;
  clear_request_bearer_token();  // before the sink, never after
  sink(
      http_status_code::FORBIDDEN,
      make_problem_detail(
          http_status_code::FORBIDDEN, "AF not authorized for this service")
          .dump());
  return true;
}

//------------------------------------------------------------------------------
bool nef_app::reject_unauthorized_nf(
    const std::string& api_name, nlohmann::json& response_body,
    int& http_code) const {
  if (authorize_nnef_request(api_name)) return false;
  http_code     = http_status_code::FORBIDDEN;
  response_body = make_problem_detail(
      http_status_code::FORBIDDEN, "NF not authorized for this Nnef service");
  return true;
}

//------------------------------------------------------------------------------
bool nef_app::reject_unauthorized_nf(
    const std::string& api_name, const response_sink& sink) const {
  if (authorize_nnef_request(api_name)) return false;
  clear_request_bearer_token();  // before the sink, never after
  sink(
      http_status_code::FORBIDDEN,
      make_problem_detail(
          http_status_code::FORBIDDEN,
          "NF not authorized for this Nnef service")
          .dump());
  return true;
}

//------------------------------------------------------------------------------
void nef_app::set_request_bearer_token(const std::string& bearer_token) const {
  g_request_bearer_token = bearer_token;
}

//------------------------------------------------------------------------------
void nef_app::clear_request_bearer_token() const {
  g_request_bearer_token.clear();
}

//------------------------------------------------------------------------------
std::string nef_app::get_request_bearer_token() const {
  return g_request_bearer_token;
}

// Subscription helpers
//------------------------------------------------------------------------------
bool nef_app::add_subscription(
    const std::string& sub_id, const std::shared_ptr<nef_subscription>& s) {
  std::unique_lock lock(m_af_subscriptions_mutex);
  m_af_sub_id2subscription[sub_id] = s;
  return true;
}

//------------------------------------------------------------------------------
bool nef_app::remove_subscription(const std::string& sub_id) {
  std::unique_lock lock(m_af_subscriptions_mutex);
  auto it = m_af_sub_id2subscription.find(sub_id);
  if (it == m_af_sub_id2subscription.end()) return false;
  m_af_sub_id2subscription.erase(it);
  return true;
}

//------------------------------------------------------------------------------
std::shared_ptr<nef_subscription> nef_app::find_subscription(
    const std::string& sub_id) const {
  std::shared_lock lock(m_af_subscriptions_mutex);
  auto it = m_af_sub_id2subscription.find(sub_id);
  if (it != m_af_sub_id2subscription.end()) return it->second;
  return nullptr;
}

//------------------------------------------------------------------------------
bool nef_app::is_subscription_owner(
    const std::shared_ptr<nef_subscription>& sub,
    const std::string& af_id) const {
  if (!sub) return false;
  const std::string owner = sub->get_scs_as_id();
  if (owner.empty()) {
    Logger::nef_app().warn(
        "Subscription owner missing for AF %s request", af_id.c_str());
    return false;
  }
  if (owner != af_id) {
    Logger::nef_app().warn(
        "AF %s is not owner of subscription (owner=%s)", af_id.c_str(),
        owner.c_str());
    return false;
  }
  return true;
}

// AF Profile helpers
//------------------------------------------------------------------------------
bool nef_app::add_af_profile(
    const std::string& af_id, const std::shared_ptr<nef_af_profile>& p) {
  std::unique_lock lock(m_af_id2profile_mutex);
  m_af_id2profile[af_id] = p;
  Logger::nef_app().debug("AF profile added: %s", af_id.c_str());
  return true;
}

//------------------------------------------------------------------------------
bool nef_app::remove_af_profile(const std::string& af_id) {
  std::unique_lock lock(m_af_id2profile_mutex);
  auto it = m_af_id2profile.find(af_id);
  if (it == m_af_id2profile.end()) return false;
  m_af_id2profile.erase(it);
  Logger::nef_app().debug("AF profile removed: %s", af_id.c_str());
  return true;
}

//------------------------------------------------------------------------------
std::shared_ptr<nef_af_profile> nef_app::find_af_profile(
    const std::string& af_id) const {
  std::shared_lock lock(m_af_id2profile_mutex);
  auto it = m_af_id2profile.find(af_id);
  if (it != m_af_id2profile.end()) return it->second;
  return nullptr;
}

//------------------------------------------------------------------------------
bool nef_app::is_af_registered(const std::string& af_id) const {
  std::shared_lock lock(m_af_id2profile_mutex);
  return m_af_id2profile.count(af_id) > 0;
}

//------------------------------------------------------------------------------
void nef_app::ensure_af_profile(
    const std::string& af_id, const std::string& sub_id) {
  auto profile = find_af_profile(af_id);
  if (!profile) {
    profile = std::make_shared<nef_af_profile>(m_event_sub);
    profile->set_af_id(af_id);
    add_af_profile(af_id, profile);
  }
  profile->add_subscription_id(sub_id);
}

//------------------------------------------------------------------------------
void nef_app::release_af_profile_subscription(
    const std::string& af_id, const std::string& sub_id) {
  auto profile = find_af_profile(af_id);
  if (!profile) return;
  profile->remove_subscription_id(sub_id);
  if (profile->has_no_subscriptions()) {
    remove_af_profile(af_id);
    Logger::nef_app().info(
        "AF %s has no remaining subscriptions – profile destroyed",
        af_id.c_str());
  }
}

// Event subscriptions
//------------------------------------------------------------------------------
void nef_app::subscribe_nf_notification() {
  auto conn = m_event_sub->subscribe_nf_notification(
      boost::bind(&nef_app::handle_nf_notification_event, this, _1, _2));
  m_connections.push_back(conn);
}

//------------------------------------------------------------------------------
void nef_app::handle_nf_notification_event(
    const std::string& nf_sub_id, const nlohmann::json& notif) {
  handle_nf_notification(nf_sub_id, notif);
}

// Inbound notification from 5GC NF
//------------------------------------------------------------------------------
bool nef_app::handle_nf_notification(
    const std::string& nf_sub_id, const nlohmann::json& notif_payload) {
  Logger::nef_app().debug(
      "Received NF notification for NF-sub-id: %s", nf_sub_id.c_str());

  // Map NF sub-id → AF sub-id.
  // PCF QoS callback: PCF appends "/notify" to evSubsc.notifUri, so the
  // extracted key is "{qos_sub_id}/notify".
  std::string af_sub_id;
  {
    std::shared_lock lock(m_nf2af_mutex);
    std::string key = nf_sub_id;
    auto it         = m_nf2af_sub_id.find(key);
    if (it == m_nf2af_sub_id.end()) {
      static const std::string kSfx = "/notify";
      if (key.size() > kSfx.size() &&
          key.compare(key.size() - kSfx.size(), kSfx.size(), kSfx) == 0) {
        key.erase(key.size() - kSfx.size());
        it = m_nf2af_sub_id.find(key);
      }
    }
    if (it == m_nf2af_sub_id.end()) {
      Logger::nef_app().warn(
          "No AF subscription found for NF sub-id: %s", nf_sub_id.c_str());
      return false;
    }
    af_sub_id = it->second;
  }

  // Find the AF subscription
  auto sub = find_subscription(af_sub_id);
  if (!sub) {
    Logger::nef_app().warn("AF subscription %s not found", af_sub_id.c_str());
    return false;
  }

  // Forward to AF — translate southbound → northbound format via mapper
  std::string af_uri = sub->get_notification_uri();
  if (!af_uri.empty()) {
    nlohmann::json t8_payload;
    bool mapped = false;
    auto svc    = sub->get_service_type();
    if (svc == nef_service_type_t::NEF_SERVICE_TYPE_MONITORING_EVENT) {
      mapped = nef_notification_mapper::amf_to_monitoring_notification(
          notif_payload, t8_payload, af_sub_id);
    } else if (svc == nef_service_type_t::NEF_SERVICE_TYPE_QOS_MONITORING) {
      // The UserPlaneNotificationData "transaction" is the AF
      // subscription's self-URI. Fall back to the AF notification URI, then to
      // the bare af_sub_id if the self-URI was not stored.
      std::string transaction = sub->get_self();
      if (transaction.empty()) transaction = af_uri;
      if (transaction.empty()) transaction = af_sub_id;
      mapped = nef_notification_mapper::pcf_to_qos_notification(
          notif_payload, t8_payload, transaction);
    } else if (svc == nef_service_type_t::NEF_SERVICE_TYPE_TRAFFIC_INFLUENCE) {
      mapped = nef_notification_mapper::pcf_to_ti_notification(
          notif_payload, t8_payload, af_sub_id);
    } else {
      // Pass-through for other service types (Analytics, PFD, BDT, etc.)
      t8_payload = notif_payload;
      mapped     = true;
    }

    if (!mapped) {
      Logger::nef_app().warn(
          "Notification mapping failed for sub %s – forwarding raw payload",
          af_sub_id.c_str());
      t8_payload = notif_payload;
    }

    auto nef_client = m_nef_client;
    const bool enqueued =
        m_notification_pool->enqueue([nef_client, af_uri, t8_payload]() {
          if (!nef_client->forward_notification_to_af(af_uri, t8_payload)) {
            Logger::nef_app().warn(
                "Failed forwarding notification to AF endpoint: %s",
                af_uri.c_str());
          }
        });
    if (!enqueued) {
      Logger::nef_app().error(
          "handle_nf_notification: notification queue full (>1000), "
          "dropping notification for AF %s. "
          "Consider increasing thread pool size.",
          af_uri.c_str());
    }
  }
  return true;
}

//------------------------------------------------------------------------------
void nef_app::handle_subscription_expiry_tick(uint64_t t) {
  (void) t;
  const auto now = std::chrono::system_clock::now();

  // Expire Nnef_EventExposure (SBI) subscriptions via eventsRepInfo.monDur
  {
    std::vector<std::string> nnef_expired;
    {
      std::shared_lock lock(m_nnef_event_subscriptions_mutex);
      for (const auto& [sub_id, sub] : m_nnef_event_subscriptions) {
        if (!sub.eventsRepInfoIsSet()) continue;
        const auto& rep = sub.getEventsRepInfo();
        if (!rep.monDurIsSet()) continue;
        std::chrono::system_clock::time_point expire_tp;
        if (!parse_monitor_expire_time(rep.getMonDur(), expire_tp)) continue;
        if (expire_tp <= now) nnef_expired.push_back(sub_id);
      }
    }
    if (!nnef_expired.empty()) {
      const std::lock_guard<std::shared_mutex> lock(
          m_nnef_event_subscriptions_mutex);
      for (const auto& sub_id : nnef_expired) {
        m_nnef_event_subscriptions.erase(sub_id);
        Logger::nef_app().info(
            "Nnef_EventExposure subscription %s expired - removing",
            sub_id.c_str());
      }
    }
  }

  std::vector<std::string> expired_sub_ids;
  {
    std::shared_lock lock(m_af_subscriptions_mutex);
    for (const auto& [sub_id, sub] : m_af_sub_id2subscription) {
      if (!sub || !sub->has_expire_time()) continue;
      if (sub->get_expire_time() <= now) {
        expired_sub_ids.push_back(sub_id);
      }
    }
  }

  for (const auto& sub_id : expired_sub_ids) {
    auto sub = find_subscription(sub_id);
    if (!sub || !sub->has_expire_time()) continue;
    if (sub->get_expire_time() > now) continue;

    const std::string nf_sub_id = sub->get_nf_subscription_id();
    const auto nf_type          = sub->get_target_nf_type();
    const auto svc_type         = sub->get_service_type();

    // Service-specific southbound cleanup before removing local state.
    // TI subscriptions store the PCF policy ID in nf_sub_id and must call
    // delete_pcf_policy_auth rather than the NF event-exposure unsubscribe
    // paths.
    if (svc_type == nef_service_type_t::NEF_SERVICE_TYPE_TRAFFIC_INFLUENCE) {
      if (!nf_sub_id.empty()) {
        uint32_t http_code_pcf = 0;
        if (!m_nef_client->delete_pcf_policy_auth(nf_sub_id, http_code_pcf)) {
          Logger::nef_app().warn(
              "F1.3: PCF TI expiry delete failed for sub=%s (http=%u)",
              sub_id.c_str(), http_code_pcf);
        }
        uint32_t http_code_udr = 0;
        if (!m_nef_client->udr_delete_influence_data(sub_id, http_code_udr)) {
          Logger::nef_app().warn(
              "F1.3: UDR TI expiry delete failed for sub=%s (http=%u)",
              sub_id.c_str(), http_code_udr);
        }
        {
          const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
          m_nf2af_sub_id.erase(nf_sub_id);
        }
      }
      {
        const std::lock_guard<std::shared_mutex> lock(m_ti_mutex);
        m_ti_sessions.erase(sub_id);
        m_ti_id2af_id.erase(sub_id);
        m_ti_id2pcf_policy_id.erase(sub_id);
      }
    } else {
      // Monitoring and QoS subscriptions: unsubscribe from the target NF.
      if (!nf_sub_id.empty()) {
        if (nf_type == nf_type_t::NF_TYPE_AMF) {
          m_nef_client->unsubscribe_amf_event_exposure(nf_sub_id);
        } else if (nf_type == nf_type_t::NF_TYPE_SMF) {
          m_nef_client->unsubscribe_smf_event_exposure(nf_sub_id);
        }
        {
          const std::lock_guard<std::shared_mutex> lock(m_nf2af_mutex);
          m_nf2af_sub_id.erase(nf_sub_id);
        }
      }
    }

    remove_subscription(sub_id);
    release_af_profile_subscription(sub->get_scs_as_id(), sub_id);
    Logger::nef_app().info(
        "Subscription %s expired - cleaning up", sub_id.c_str());
  }
}
