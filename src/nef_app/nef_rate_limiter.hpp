/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

/**
 * @file nef_rate_limiter.hpp
 * @brief F4.2 — Per-AF token-bucket rate limiter (header-only).
 *
 * Each AF (identified by bearer token or remote address) maintains an
 * independent token bucket. Tokens refill continuously based on elapsed time
 * since the last check. When a bucket is empty `allow()` returns false and
 * the caller should respond 429 Too Many Requests.
 *
 * Thread-safety: a single std::mutex guards the per-bucket map. For typical
 * NEF loads (O(10) AFs) contention is negligible.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace oai::nef::app {

class nef_rate_limiter {
 public:
  /// @param tokens_per_second  Refill rate (sustained request rate per AF).
  /// @param max_tokens         Burst capacity (initial bucket fill level too).
  explicit nef_rate_limiter(
      double tokens_per_second = 100.0, double max_tokens = 200.0)
      : m_tps(tokens_per_second), m_max(max_tokens) {}

  nef_rate_limiter(const nef_rate_limiter&) = delete;
  nef_rate_limiter& operator=(const nef_rate_limiter&) = delete;

  /// Process-wide singleton.
  static nef_rate_limiter& instance() {
    static nef_rate_limiter inst;
    return inst;
  }

  /// Consume one token from @p af_id's bucket.
  /// @returns true if the request is allowed, false if rate-limited.
  bool allow(const std::string& af_id) {
    const auto now = clock_t::now();
    std::lock_guard<std::mutex> lk(m_mu);
    auto& bkt = m_buckets[af_id];
    // First access: initialise bucket.
    if (!bkt.initialised) {
      bkt.tokens = m_max;
      bkt.last_refill = now;
      bkt.initialised = true;
    } else {
      // Refill proportionally to elapsed time.
      const auto elapsed =
          std::chrono::duration<double>(now - bkt.last_refill).count();
      bkt.tokens = std::min(m_max, bkt.tokens + elapsed * m_tps);
      bkt.last_refill = now;
    }
    if (bkt.tokens < 1.0) return false;
    bkt.tokens -= 1.0;
    return true;
  }

  /// Reconfigure rate for future bucket initialisations.
  /// Existing buckets retain their current token count but adopt the new rate
  /// from the next call onwards.
  void set_config(double tokens_per_second, double max_tokens) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_tps = tokens_per_second;
    m_max = max_tokens;
  }

  /// Remove all buckets (useful in tests to reset state between cases).
  void clear() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_buckets.clear();
  }

  /// Remove the bucket for a single AF (e.g., after logout).
  void invalidate(const std::string& af_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_buckets.erase(af_id);
  }

 private:
  using clock_t = std::chrono::steady_clock;

  struct bucket {
    double tokens       = 0.0;
    clock_t::time_point last_refill;
    bool initialised    = false;
  };

  double                                 m_tps;
  double                                 m_max;
  std::mutex                             m_mu;
  std::unordered_map<std::string, bucket> m_buckets;
};

}  // namespace oai::nef::app
