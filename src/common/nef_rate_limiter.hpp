/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
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
  /// tokens_per_second is the sustained per-AF rate; max_tokens is the burst
  /// capacity, and also how full a new bucket starts out.
  explicit nef_rate_limiter(
      double tokens_per_second = 100.0, double max_tokens = 200.0)
      : m_tps(tokens_per_second), m_max(max_tokens) {}

  nef_rate_limiter(const nef_rate_limiter&) = delete;
  nef_rate_limiter& operator=(const nef_rate_limiter&) = delete;

  static nef_rate_limiter& instance() {
    static nef_rate_limiter inst;
    return inst;
  }

  /// Spend one of this AF's tokens. False means it has run out.
  bool allow(const std::string& af_id) {
    const auto now = clock_t::now();
    std::lock_guard<std::mutex> lk(m_mu);
    auto& bkt = m_buckets[af_id];
    if (!bkt.initialised) {
      bkt.tokens      = m_max;
      bkt.last_refill = now;
      bkt.initialised = true;
    } else {
      // Refill proportionally to elapsed time.
      const auto elapsed =
          std::chrono::duration<double>(now - bkt.last_refill).count();
      bkt.tokens      = std::min(m_max, bkt.tokens + elapsed * m_tps);
      bkt.last_refill = now;
    }
    if (bkt.tokens < 1.0) return false;
    bkt.tokens -= 1.0;
    return true;
  }

  /// Change the rate. Buckets that already exist keep whatever tokens they
  /// are holding, but refill at the new rate from here on.
  void set_config(double tokens_per_second, double max_tokens) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_tps = tokens_per_second;
    m_max = max_tokens;
  }

  /// Mainly for resetting state between tests.
  void clear() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_buckets.clear();
  }

  /// Forget one AF, for instance after it logs out.
  void invalidate(const std::string& af_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_buckets.erase(af_id);
  }

 private:
  using clock_t = std::chrono::steady_clock;

  struct bucket {
    double tokens = 0.0;
    clock_t::time_point last_refill;
    bool initialised = false;
  };

  double m_tps;
  double m_max;
  std::mutex m_mu;
  std::unordered_map<std::string, bucket> m_buckets;
};

}  // namespace oai::nef::app
