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

namespace oai::nef::app {

// ── Circuit-breaker state machine ─────────────────────────────────────────────

/// Three-state circuit-breaker FSM per NF type.
enum class sbi_cb_state {
  CLOSED,     ///< Normal operation; all calls go through.
  OPEN,       ///< Failing fast; calls are rejected until cooldown expires.
  HALF_OPEN,  ///< One probe allowed through to test NF recovery.
};

/// Internal per-NF tracking entry (not exposed in the public API).
struct sbi_cb_entry_t {
  sbi_cb_state state                                 = sbi_cb_state::CLOSED;
  int          consecutive_failures                  = 0;
  std::chrono::steady_clock::time_point open_since{};
  bool         half_open_probe_in_flight             = false;
};

/**
 * Thread-safe per-NF-type three-state circuit breaker.
 *
 * Keys are NF type strings: "AMF", "SMF", "PCF", "UDR".
 *
 * State transitions:
 *   CLOSED    → OPEN      after m_threshold consecutive failures.
 *   OPEN      → HALF_OPEN automatically when is_open() is called and
 *                          m_cooldown_secs have elapsed since the CB opened.
 *   HALF_OPEN → CLOSED    on the next record_success().
 *   HALF_OPEN → OPEN      on the next record_failure() (probe failed).
 *
 * Freely constructible so unit tests can create fully isolated instances with
 * custom threshold / cooldown.  Production code uses the singleton via
 * sbi_circuit_breaker_registry::instance().
 */
class sbi_circuit_breaker_registry {
 public:
  static constexpr int CB_THRESHOLD_DEFAULT  = 5;
  static constexpr int COOLDOWN_SECS_DEFAULT = 30;

  /**
   * @param cb_threshold   Consecutive failures before CLOSED→OPEN transition.
   * @param cooldown_secs  Seconds in OPEN state before transitioning to HALF_OPEN.
   */
  explicit sbi_circuit_breaker_registry(
      int cb_threshold  = CB_THRESHOLD_DEFAULT,
      int cooldown_secs = COOLDOWN_SECS_DEFAULT)
      : m_threshold(cb_threshold), m_cooldown_secs(cooldown_secs) {}

  /// Returns the process-wide singleton (production use).
  static sbi_circuit_breaker_registry& instance() {
    static sbi_circuit_breaker_registry self;
    return self;
  }

  /**
   * If the NF is in OPEN state and the cooldown has elapsed, transition it to
   * HALF_OPEN so the next probe can go through.  Safe to call concurrently.
   */
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
      e.state                      = sbi_cb_state::HALF_OPEN;
      e.half_open_probe_in_flight = false;
    }
  }

  /**
   * Returns true when calls to the NF should be blocked (state == OPEN and
   * the cooldown has not yet expired).  Automatically transitions OPEN →
   * HALF_OPEN when the cooldown expires.
   */
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

  /**
   * Record a successful NF call.  Transitions any state back to CLOSED and
   * resets the consecutive-failure counter.
   */
  void record_success(const std::string& nf_type) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto& e               = m_entries[nf_type];
    e.state               = sbi_cb_state::CLOSED;
    e.consecutive_failures = 0;
    e.half_open_probe_in_flight = false;
  }

  /**
   * Record a failed NF call.
   * - HALF_OPEN: probe failed → transition back to OPEN and restart the timer.
   * - CLOSED: increment counter; if threshold reached → transition to OPEN.
   */
  void record_failure(const std::string& nf_type) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(m_mtx);
    auto& e = m_entries[nf_type];

    if (e.state == sbi_cb_state::HALF_OPEN) {
      // Probe failed: back to OPEN, restart cooldown timer.
      e.state                      = sbi_cb_state::OPEN;
      e.open_since                 = now;
      e.half_open_probe_in_flight = false;
      return;
    }

    ++e.consecutive_failures;
    if (e.state == sbi_cb_state::CLOSED &&
        e.consecutive_failures >= m_threshold) {
      e.state                      = sbi_cb_state::OPEN;
      e.open_since                 = now;
      e.half_open_probe_in_flight = false;
    }
  }

  /// Returns the raw FSM state for a given NF type (for diagnostics / tests).
  sbi_cb_state get_state(const std::string& nf_type) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_entries.find(nf_type);
    if (it == m_entries.end()) return sbi_cb_state::CLOSED;
    return it->second.state;
  }

  /// Clears all entries.  For test isolation only.
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

// ── Retry policy helpers ───────────────────────────────────────────────────────

/**
 * Returns true if the given HTTP status code warrants a retry.
 *
 * @param status   HTTP status code; 0 means connection-level failure.
 * @param is_post  True for POST operations — only connection-level failures
 *                 (status == 0) are retried to avoid duplicate resource
 *                 creation.  On 503/429, callers were reached and returning
 *                 a response, so re-POSTing would risk double-creation.
 */
inline bool sbi_should_retry(int status, bool is_post) {
  // Assumes caller has already handled 2xx (success) and 4xx (permanent error).
  if (is_post) return (status == 0);  // POST: only retry on connection failure
  return (status == 0 || status == 503 || status == 429);
}

// ── sbi_call_with_retry ───────────────────────────────────────────────────────

/**
 * Invoke an SBI callable with retry-with-backoff and circuit-breaker
 * integration.
 *
 * Retry policy:
 *  - Up to @p max_attempts total (default 4 = 1 initial + 3 retries).
 *  - Backoff: 200 ms × 2^(attempt-1) with ±10 % jitter →
 *      attempt 1→2: 180–220 ms
 *      attempt 2→3: 360–440 ms
 *      attempt 3→4: 720–880 ms
 *  - Retriable statuses: 0 (connection failure), 503, 429.
 *    POST operations only retry on status 0.
 *  - 4xx errors are returned immediately with no CB update (application-level,
 *    not a signal of server health).
 *  - All other non-retriable failures update the circuit breaker and return.
 *
 * Circuit breaker integration:
 *  - If the CB reports OPEN for @p nf_type, the call is fast-failed and -1 is
 *    returned without invoking @p attempt_fn.
 *  - On success: record_success() resets the failure counter.
 *  - On failure (after all retries or non-retriable): record_failure().
 *
 * @tparam CallFn   Callable returning int (HTTP status code; 0 = network error).
 * @tparam SleepFn  void(std::chrono::milliseconds) — injected for test speed.
 * @tparam LogFn    void(const std::string&)         — injected for test isolation.
 *
 * @param nf_type      NF type string used as the CB key ("AMF", "SMF", …).
 * @param is_post      True for POST operations (relaxed retry policy).
 * @param attempt_fn   The SBI call to execute; returns HTTP status or 0.
 * @param cb           Circuit-breaker registry to consult and update.
 * @param sleep_fn     Called with the delay before each retry attempt.
 * @param log_fn       Called for notable retry and circuit-breaker events.
 * @param max_attempts Total number of attempts including the first (default 4).
 *
 * @return Final HTTP status code, or -1 if the circuit breaker blocked the call.
 */
template<typename CallFn, typename SleepFn, typename LogFn>
int sbi_call_with_retry(
    const std::string&            nf_type,
    bool                          is_post,
    CallFn&&                      attempt_fn,
    sbi_circuit_breaker_registry& cb,
    SleepFn&&                     sleep_fn,
    LogFn&&                       log_fn,
    int                           max_attempts = 4) {
  // ── Circuit-breaker fast path ─────────────────────────────────────────────
  if (cb.is_open(nf_type)) {
    log_fn("[SBI] Circuit breaker OPEN for NF " + nf_type +
           " — dropping call without sending");
    return -1;
  }

  // ── Per-call jitter RNG ───────────────────────────────────────────────────
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

      log_fn("[SBI] Retry attempt " + std::to_string(attempt + 1) +
             "/" + std::to_string(max_attempts) + " for NF " + nf_type +
             " (delay " + std::to_string(delay_ms) + " ms)");

      sleep_fn(std::chrono::milliseconds(delay_ms));
    }

    try {
      status = attempt_fn();
    } catch (...) {
      status = 0;  // treat thrown exception as connection failure → retriable
    }

    // ── 2xx: success ────────────────────────────────────────────────────────
    if (status >= 200 && status < 300) {
      cb.record_success(nf_type);
      return status;
    }

    // ── 4xx: permanent application-level error, no CB update ────────────────
    if (status >= 400 && status < 500) {
      return status;
    }

    // ── Check whether this failure warrants a retry ──────────────────────────
    if (!sbi_should_retry(status, is_post)) {
      // Non-retriable transient error (e.g. POST + 503): record failure and bail.
      cb.record_failure(nf_type);
      return status;
    }

    // Retriable: continue to next attempt.
  }

  // ── All attempts exhausted ────────────────────────────────────────────────
  cb.record_failure(nf_type);
  return status;
}

}  // namespace oai::nef::app
