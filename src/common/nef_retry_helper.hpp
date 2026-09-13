/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "3gpp_29.500.h"

namespace oai::nef::app {

// The _LEVEL suffixes are there to dodge the WARN/ERROR/CRITICAL macros in
// syslog.h.
enum class retry_log_level { WARN_LEVEL, CRIT_LEVEL, ERR_LEVEL };

using retry_log_fn = std::function<void(retry_log_level, const std::string&)>;

/**
 * Per-endpoint consecutive-failure counter, safe to share across threads.
 *
 * CB_THRESHOLD failures in a row trips the endpoint "open" and callers should
 * stop sending to it; a single success clears the count again.
 *
 * Production code uses the instance() singleton. The constructor is public so
 * tests can hold their own isolated registry.
 */
class circuit_breaker_registry {
 public:
  static constexpr int CB_THRESHOLD = 10;

  circuit_breaker_registry() = default;

  static circuit_breaker_registry& instance() {
    static circuit_breaker_registry self;
    return self;
  }

  bool is_open(const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_fails.find(endpoint);
    return it != m_fails.end() && it->second >= CB_THRESHOLD;
  }

  /// Returns the new count.
  int record_failure(const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(m_mtx);
    return ++m_fails[endpoint];
  }

  void record_success(const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_fails.erase(endpoint);
  }

  /// Mainly for test isolation.
  void reset_all() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_fails.clear();
  }

 private:
  mutable std::mutex m_mtx;
  std::unordered_map<std::string, int> m_fails;
};

/**
 * The circuit-breaker key for a URI: its scheme, host and port, so that every
 * path on one AF shares a single breaker.
 *   "http://af.example.com:8080/notify/v1" -> "http://af.example.com:8080"
 * A URI with no path comes back unchanged.
 */
inline std::string cb_endpoint_key(const std::string& uri) {
  const auto proto_end = uri.find("://");
  if (proto_end == std::string::npos) return uri;
  const auto path_start = uri.find('/', proto_end + 3);
  return (path_start == std::string::npos) ? uri : uri.substr(0, path_start);
}

/**
 * Call attempt_fn() until it succeeds, backing off 1s, 2s, 4s... between
 * tries. attempt_fn returns an HTTP status, or 0 for a network error.
 *
 * A 5xx or a network error is worth retrying; a 4xx is not, and fails
 * immediately. If the endpoint's breaker is already open nothing is sent at
 * all. Success clears the breaker, and any final failure feeds it.
 *
 * Sleeping and logging are injected rather than called directly, so the tests
 * run instantly and need no Logger singleton.
 */
inline bool retry_with_backoff(
    const std::string& endpoint, std::function<int()> attempt_fn,
    retry_log_fn log_fn, std::function<void(std::chrono::seconds)> sleep_fn,
    int max_attempts, circuit_breaker_registry& cb) {
  if (cb.is_open(endpoint)) {
    log_fn(
        retry_log_level::CRIT_LEVEL,
        "[CRITICAL] Circuit breaker OPEN for endpoint " + endpoint +
            " — dropping notification without sending");
    return false;
  }

  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    // No delay before the first try.
    if (attempt > 0) {
      const int delay_secs = 1 << (attempt - 1);  // 1 s, 2 s, 4 s ...
      log_fn(
          retry_log_level::WARN_LEVEL, "Retry attempt " +
                                           std::to_string(attempt + 1) +
                                           " for endpoint " + endpoint);
      sleep_fn(std::chrono::seconds(delay_secs));
    }

    int status = 0;
    try {
      status = attempt_fn();
    } catch (...) {
      status = 0;  // treat exception as network failure → retriable
    }

    if (status >= http_status_code::OK &&
        status < http_status_code::MULTIPLE_CHOICES) {
      cb.record_success(endpoint);
      return true;
    }

    // A 4xx is the AF telling us the request itself is wrong; retrying it
    // would just repeat the same mistake.
    if (status >= http_status_code::BAD_REQUEST &&
        status < http_status_code::INTERNAL_SERVER_ERROR) {
      const int fails = cb.record_failure(endpoint);
      if (fails >= circuit_breaker_registry::CB_THRESHOLD) {
        log_fn(
            retry_log_level::CRIT_LEVEL,
            "[CRITICAL] Circuit breaker OPENED for endpoint " + endpoint +
                " after " + std::to_string(fails) + " consecutive failures");
      } else {
        log_fn(
            retry_log_level::ERR_LEVEL,
            "Permanent 4xx failure (status=" + std::to_string(status) +
                ") for endpoint " + endpoint + " — not retrying");
      }
      return false;
    }

    // Anything else (5xx, or status 0 for a network error) is transient, so
    // fall through and try again.
  }

  const int fails = cb.record_failure(endpoint);
  if (fails >= circuit_breaker_registry::CB_THRESHOLD) {
    log_fn(
        retry_log_level::CRIT_LEVEL,
        "[CRITICAL] Circuit breaker OPENED for endpoint " + endpoint +
            " after " + std::to_string(fails) + " consecutive failures");
  } else {
    log_fn(
        retry_log_level::ERR_LEVEL, "All " + std::to_string(max_attempts) +
                                        " attempts failed for endpoint " +
                                        endpoint);
  }
  return false;
}

}  // namespace oai::nef::app
