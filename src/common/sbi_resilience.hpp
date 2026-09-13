/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>

#include "3gpp_29.500.h"

namespace oai::nef::app {

/// Per-NF-type circuit-breaker state.
enum class sbi_cb_state {
  CLOSED,     ///< Normal operation; all calls go through.
  OPEN,       ///< Failing fast; calls are rejected until cooldown expires.
  HALF_OPEN,  ///< One probe allowed through to test NF recovery.
};

struct sbi_cb_entry_t {
  sbi_cb_state state       = sbi_cb_state::CLOSED;
  int consecutive_failures = 0;
  std::chrono::steady_clock::time_point open_since{};
  bool half_open_probe_in_flight = false;
};

/**
 * Circuit breaker per NF type ("AMF", "SMF", "PCF", "UDR"), safe to share
 * across threads.
 *
 *   CLOSED    -> OPEN       after m_threshold consecutive failures.
 *   OPEN      -> HALF_OPEN  once the cooldown elapses, noticed lazily on the
 *                           next is_open() call rather than by a timer.
 *   HALF_OPEN -> CLOSED     the probe succeeded.
 *   HALF_OPEN -> OPEN       the probe failed; the cooldown starts over.
 *
 * Production code uses the instance() singleton. The constructor is public so
 * tests can hold their own registry with a custom threshold and cooldown.
 */
class sbi_circuit_breaker_registry {
 public:
  static constexpr int CB_THRESHOLD_DEFAULT  = 5;
  static constexpr int COOLDOWN_SECS_DEFAULT = 30;

  explicit sbi_circuit_breaker_registry(
      int cb_threshold  = CB_THRESHOLD_DEFAULT,
      int cooldown_secs = COOLDOWN_SECS_DEFAULT)
      : m_threshold(cb_threshold), m_cooldown_secs(cooldown_secs) {}

  static sbi_circuit_breaker_registry& instance() {
    static sbi_circuit_breaker_registry self;
    return self;
  }

  /// Move an OPEN entry whose cooldown has elapsed to HALF_OPEN, so one
  /// probe can get through. Safe to call concurrently.
  void transition_half_open_if_ready(const std::string& nf_type) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_entries.find(nf_type);
    if (it == m_entries.end()) return;
    auto& e = it->second;
    if (e.state != sbi_cb_state::OPEN) return;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - e.open_since)
            .count();
    if (elapsed >= m_cooldown_secs) {
      e.state                     = sbi_cb_state::HALF_OPEN;
      e.half_open_probe_in_flight = false;
    }
  }

  /// True while calls should be blocked. Also does the lazy OPEN ->
  /// HALF_OPEN promotion once the cooldown is up.
  bool is_open(const std::string& nf_type) {
    transition_half_open_if_ready(nf_type);
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_entries.find(nf_type);
    if (it == m_entries.end()) return false;
    auto& e = it->second;
    if (e.state == sbi_cb_state::OPEN) return true;
    if (e.state == sbi_cb_state::HALF_OPEN) {
      if (!e.half_open_probe_in_flight) {
        // Allow exactly one recovery probe in HALF_OPEN.
        e.half_open_probe_in_flight = true;
        return false;
      }
      return true;
    }
    return false;
  }

  /// Any success closes the breaker and clears the failure count.
  void record_success(const std::string& nf_type) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto& e                     = m_entries[nf_type];
    e.state                     = sbi_cb_state::CLOSED;
    e.consecutive_failures      = 0;
    e.half_open_probe_in_flight = false;
  }

  /// A failure while HALF_OPEN reopens the breaker; otherwise it counts
  /// towards the threshold.
  void record_failure(const std::string& nf_type) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(m_mtx);
    auto& e = m_entries[nf_type];

    if (e.state == sbi_cb_state::HALF_OPEN) {
      // Probe failed: back to OPEN, restart cooldown timer.
      e.state                     = sbi_cb_state::OPEN;
      e.open_since                = now;
      e.half_open_probe_in_flight = false;
      return;
    }

    ++e.consecutive_failures;
    if (e.state == sbi_cb_state::CLOSED &&
        e.consecutive_failures >= m_threshold) {
      e.state                     = sbi_cb_state::OPEN;
      e.open_since                = now;
      e.half_open_probe_in_flight = false;
    }
  }

  /// For diagnostics and tests.
  sbi_cb_state get_state(const std::string& nf_type) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_entries.find(nf_type);
    if (it == m_entries.end()) return sbi_cb_state::CLOSED;
    return it->second.state;
  }

  /// For test isolation.
  void reset_all() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_entries.clear();
  }

 private:
  mutable std::mutex m_mtx;
  std::unordered_map<std::string, sbi_cb_entry_t> m_entries;
  int m_threshold;
  int m_cooldown_secs;
};

/**
 * Whether a status is worth retrying. status 0 means the connection itself
 * failed.
 *
 * POSTs are treated more cautiously: only a failed connection is retried. A
 * 503 or 429 means the NF did receive the request and answered, so sending it
 * again risks creating the resource twice.
 */
inline bool sbi_should_retry(int status, bool is_post) {
  // Assumes caller has already handled 2xx (success) and 4xx (permanent error).
  if (is_post) return (status == 0);  // POST: only retry on connection failure
  return (
      status == 0 || status == http_status_code::SERVICE_UNAVAILABLE ||
      status == http_status_code::TOO_MANY_REQUESTS);
}

/**
 * Run an SBI call with backoff and circuit-breaker handling, returning the
 * final HTTP status — or -1 if the breaker was open and nothing was sent.
 *
 * Retries back off 200ms, 400ms, 800ms, each with +/-10% jitter so a fleet of
 * NEFs does not resynchronise onto a recovering NF. Only status 0, 503 and
 * 429 are retried, and POSTs narrow that to status 0 alone.
 *
 * A 4xx returns straight away and leaves the breaker untouched: it says the
 * request was wrong, not that the NF is unhealthy. Every other failure feeds
 * the breaker, and success clears it.
 *
 * Sleeping and logging are injected so tests run fast and in isolation.
 */
template<typename CallFn, typename SleepFn, typename LogFn>
int sbi_call_with_retry(
    const std::string& nf_type, bool is_post, CallFn&& attempt_fn,
    sbi_circuit_breaker_registry& cb, SleepFn&& sleep_fn, LogFn&& log_fn,
    int max_attempts = 4) {
  if (cb.is_open(nf_type)) {
    log_fn(
        "[SBI] Circuit breaker OPEN for NF " + nf_type +
        " — dropping call without sending");
    return -1;
  }

  std::mt19937 rng{std::random_device{}()};

  int status = 0;

  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (attempt > 0) {
      // Exponential backoff with ±10 % jitter.
      // Base delays: 200 ms, 400 ms, 800 ms …
      const int base_ms     = 200 * (1 << (attempt - 1));
      const int jitter_span = base_ms / 10;  // 10 % of base
      std::uniform_int_distribution<int> jitter_dist(-jitter_span, jitter_span);
      const int delay_ms = base_ms + jitter_dist(rng);

      log_fn(
          "[SBI] Retry attempt " + std::to_string(attempt + 1) + "/" +
          std::to_string(max_attempts) + " for NF " + nf_type + " (delay " +
          std::to_string(delay_ms) + " ms)");

      sleep_fn(std::chrono::milliseconds(delay_ms));
    }

    try {
      status = attempt_fn();
    } catch (...) {
      status = 0;  // treat thrown exception as connection failure → retriable
    }

    // 2xx: success
    if (status >= http_status_code::OK &&
        status < http_status_code::MULTIPLE_CHOICES) {
      cb.record_success(nf_type);
      return status;
    }

    // 4xx: permanent application-level error, no CB update
    if (status >= http_status_code::BAD_REQUEST &&
        status < http_status_code::INTERNAL_SERVER_ERROR) {
      return status;
    }

    // Check whether this failure warrants a retry
    if (!sbi_should_retry(status, is_post)) {
      // Non-retriable transient error (e.g. POST + 503): record failure and
      // bail.
      cb.record_failure(nf_type);
      return status;
    }

    // Retriable: continue to next attempt.
  }

  // All attempts exhausted
  cb.record_failure(nf_type);
  return status;
}

}  // namespace oai::nef::app
