/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace oai::nef::app {

// ── Log level used by the retry helper ───────────────────────────────────────
// Deliberately avoids the names WARN/ERROR/CRITICAL to prevent collisions with
// system macros from syslog.h / <windows.h>.
enum class retry_log_level { WARN_LEVEL, CRIT_LEVEL, ERR_LEVEL };

/// Callback type for logging retry and circuit-breaker events.
using retry_log_fn = std::function<void(retry_log_level, const std::string&)>;

// ── Circuit-breaker registry ──────────────────────────────────────────────────

/**
 * Thread-safe per-endpoint consecutive-failure counter.
 *
 * When the count for an endpoint reaches CB_THRESHOLD consecutive failures the
 * endpoint is considered "open": callers should skip sending and log CRITICAL.
 * Any success resets the counter to zero.
 *
 * Freely constructible so unit tests can create fully isolated instances;
 * the free-function `circuit_breaker_registry::instance()` exposes the
 * process-wide singleton for production code.
 */
class circuit_breaker_registry {
 public:
  static constexpr int CB_THRESHOLD = 10;

  circuit_breaker_registry() = default;

  /// Returns the process-wide singleton instance.
  static circuit_breaker_registry& instance() {
    static circuit_breaker_registry self;
    return self;
  }

  /// Returns true when the consecutive-failure count >= CB_THRESHOLD.
  bool is_open(const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_fails.find(endpoint);
    return it != m_fails.end() && it->second >= CB_THRESHOLD;
  }

  /// Increments the failure counter and returns the new count.
  int record_failure(const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(m_mtx);
    return ++m_fails[endpoint];
  }

  /// Resets the failure counter for @p endpoint (call on any success).
  void record_success(const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_fails.erase(endpoint);
  }

  /// Clears all counters.  Primarily for test isolation.
  void reset_all() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_fails.clear();
  }

 private:
  mutable std::mutex m_mtx;
  std::unordered_map<std::string, int> m_fails;
};

// ── URI helpers ───────────────────────────────────────────────────────────────

/**
 * Extract the scheme+host+port prefix of a URI to use as the circuit-breaker
 * key.  Example:
 *   "http://af.example.com:8080/notify/v1" → "http://af.example.com:8080"
 * Falls back to the full URI when no path separator is found.
 */
inline std::string cb_endpoint_key(const std::string& uri) {
  const auto proto_end = uri.find("://");
  if (proto_end == std::string::npos) return uri;
  const auto path_start = uri.find('/', proto_end + 3);
  return (path_start == std::string::npos) ? uri : uri.substr(0, path_start);
}

// ── Retry-with-backoff ────────────────────────────────────────────────────────

/**
 * Attempt `attempt_fn()` up to `max_attempts` times with exponential backoff
 * between consecutive tries.
 *
 * Retry policy:
 *  - Retry on network failure (status == 0 / thrown exception) or 5xx.
 *  - Do NOT retry on 4xx — return false immediately.
 *  - Backoff delays: 1 s before attempt 2, 2 s before attempt 3, etc.
 *
 * Circuit breaker:
 *  - If the registry reports the endpoint is already open, skip the attempt,
 *    log CRITICAL and return false without sending.
 *  - On any success: reset the per-endpoint failure counter.
 *  - On any final failure (4xx or exhausted retries): increment counter;
 *    log CRITICAL when it reaches CB_THRESHOLD.
 *
 * All I/O side-effects (sleeping, logging) are injected so unit tests can run
 * instantly without a live server or a Logger singleton.
 *
 * @param endpoint      AF base-URL, used as the circuit-breaker map key.
 * @param attempt_fn    Returns HTTP status code; return 0 for network error.
 * @param log_fn        Called with (level, message) for notable events.
 * @param sleep_fn      Called with the delay duration before each retry.
 * @param max_attempts  Total attempts (1 initial + (max_attempts-1) retries).
 * @param cb            Circuit-breaker registry to consult / update.
 * @return true on success (any 2xx response).
 */
inline bool retry_with_backoff(
    const std::string& endpoint,
    std::function<int()> attempt_fn,
    retry_log_fn log_fn,
    std::function<void(std::chrono::seconds)> sleep_fn,
    int max_attempts,
    circuit_breaker_registry& cb) {
  // ── Circuit-breaker fast path ─────────────────────────────────────────────
  if (cb.is_open(endpoint)) {
    log_fn(retry_log_level::CRIT_LEVEL,
           "[CRITICAL] Circuit breaker OPEN for endpoint " + endpoint +
               " — dropping notification without sending");
    return false;
  }

  // ── Attempt loop ──────────────────────────────────────────────────────────
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    // Exponential backoff before retry attempts (not before the first try).
    if (attempt > 0) {
      const int delay_secs = 1 << (attempt - 1);  // 1 s, 2 s, 4 s ...
      log_fn(retry_log_level::WARN_LEVEL,
             "Retry attempt " + std::to_string(attempt + 1) +
                 " for endpoint " + endpoint);
      sleep_fn(std::chrono::seconds(delay_secs));
    }

    int status = 0;
    try {
      status = attempt_fn();
    } catch (...) {
      status = 0;  // treat exception as network failure → retriable
    }

    // 2xx → success: reset circuit breaker and return
    if (status >= 200 && status < 300) {
      cb.record_success(endpoint);
      return true;
    }

    // 4xx → permanent application-level error: no point retrying
    if (status >= 400 && status < 500) {
      const int fails = cb.record_failure(endpoint);
      if (fails >= circuit_breaker_registry::CB_THRESHOLD) {
        log_fn(retry_log_level::CRIT_LEVEL,
               "[CRITICAL] Circuit breaker OPENED for endpoint " + endpoint +
                   " after " + std::to_string(fails) +
                   " consecutive failures");
      } else {
        log_fn(retry_log_level::ERR_LEVEL,
               "Permanent 4xx failure (status=" + std::to_string(status) +
                   ") for endpoint " + endpoint + " — not retrying");
      }
      return false;
    }

    // 5xx or network failure (status == 0) → transient; loop continues
  }

  // ── All attempts exhausted ────────────────────────────────────────────────
  const int fails = cb.record_failure(endpoint);
  if (fails >= circuit_breaker_registry::CB_THRESHOLD) {
    log_fn(retry_log_level::CRIT_LEVEL,
           "[CRITICAL] Circuit breaker OPENED for endpoint " + endpoint +
               " after " + std::to_string(fails) + " consecutive failures");
  } else {
    log_fn(retry_log_level::ERR_LEVEL,
           "All " + std::to_string(max_attempts) +
               " attempts failed for endpoint " + endpoint);
  }
  return false;
}

}  // namespace oai::nef::app
