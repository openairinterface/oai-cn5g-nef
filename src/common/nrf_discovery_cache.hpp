/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace oai::nef::app {

/// How long a discovered endpoint stays usable, in seconds.
static constexpr int NRF_CACHE_DEFAULT_TTL_SECS = 30;

/**
 * NF-type to endpoint URL, with a TTL, safe to share across threads.
 *
 * Production code uses the instance() singleton. The constructor is public so
 * tests can hold their own isolated cache.
 */
class nrf_discovery_cache {
 public:
  nrf_discovery_cache() = default;

  static nrf_discovery_cache& instance() {
    static nrf_discovery_cache self;
    return self;
  }

  /// Look up an endpoint by NF type ("AMF", "SMF", "PCF", "UDR"). An expired
  /// entry counts as a miss.
  bool get(const std::string& nf_type, std::string& out_endpoint) const {
    const auto now = std::chrono::steady_clock::now();
    std::shared_lock<std::shared_mutex> lk(m_mtx);
    auto it = m_entries.find(nf_type);
    if (it == m_entries.end()) return false;
    if (now >= it->second.expires_at) return false;  // expired
    out_endpoint = it->second.endpoint;
    return true;
  }

  /// Store an endpoint, replacing any existing one. A ttl of 0 expires
  /// immediately, which is handy in tests.
  void put(
      const std::string& nf_type, std::string endpoint,
      int ttl_seconds = NRF_CACHE_DEFAULT_TTL_SECS) {
    const auto expires =
        std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
    std::unique_lock<std::shared_mutex> lk(m_mtx);
    m_entries[nf_type] = {std::move(endpoint), expires};
  }

  /// Drop an entry, so the next request rediscovers. Worth calling when a
  /// cached endpoint answers with a connection error or a 503 — the NF has
  /// probably moved.
  void invalidate(const std::string& nf_type) {
    std::unique_lock<std::shared_mutex> lk(m_mtx);
    m_entries.erase(nf_type);
  }

  /// Mainly for test teardown.
  void clear() {
    std::unique_lock<std::shared_mutex> lk(m_mtx);
    m_entries.clear();
  }

 private:
  struct cache_entry {
    std::string endpoint;
    std::chrono::steady_clock::time_point expires_at;
  };

  mutable std::shared_mutex m_mtx;
  std::unordered_map<std::string, cache_entry> m_entries;
};

}  // namespace oai::nef::app
