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

/// Default TTL (seconds) for a cached NRF discovery result.
static constexpr int NRF_CACHE_DEFAULT_TTL_SECS = 30;

/**
 * Thread-safe TTL-based cache: NF-type string → endpoint URL.
 *
 * Freely constructible so unit tests can create fully isolated instances.
 * Production code uses the process-wide singleton via instance().
 */
class nrf_discovery_cache {
 public:
  nrf_discovery_cache() = default;

  /// Returns the process-wide singleton (production use).
  static nrf_discovery_cache& instance() {
    static nrf_discovery_cache self;
    return self;
  }

  /**
   * Look up a cached endpoint for @p nf_type.
   *
   * @param nf_type       NF-type key (e.g. "AMF", "SMF", "PCF", "UDR").
   * @param out_endpoint  Set to the cached endpoint on a cache hit.
   * @return true if a valid (non-expired) entry exists; false on a miss or
   *         expired entry.
   */
  bool get(const std::string& nf_type, std::string& out_endpoint) const {
    const auto now = std::chrono::steady_clock::now();
    std::shared_lock<std::shared_mutex> lk(m_mtx);
    auto it = m_entries.find(nf_type);
    if (it == m_entries.end()) return false;
    if (now >= it->second.expires_at) return false;  // expired
    out_endpoint = it->second.endpoint;
    return true;
  }

  /**
   * Store (or overwrite) an endpoint for @p nf_type.
   *
   * @param nf_type     NF-type key.
   * @param endpoint    The discovered endpoint URL to cache.
   * @param ttl_seconds Lifetime in seconds (default: NRF_CACHE_DEFAULT_TTL_SECS).
   *                    Pass 0 for an entry that expires immediately (useful in
   *                    tests).
   */
  void put(const std::string& nf_type,
           std::string        endpoint,
           int                ttl_seconds = NRF_CACHE_DEFAULT_TTL_SECS) {
    const auto expires =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(ttl_seconds);
    std::unique_lock<std::shared_mutex> lk(m_mtx);
    m_entries[nf_type] = {std::move(endpoint), expires};
  }

  /**
   * Remove the cached entry for @p nf_type, if any.
   * Call this when a subsequent SBI call using the cached endpoint returns
   * status 0 (connection error) or 503 (service unavailable), forcing fresh
   * NRF discovery on the next request.
   */
  void invalidate(const std::string& nf_type) {
    std::unique_lock<std::shared_mutex> lk(m_mtx);
    m_entries.erase(nf_type);
  }

  /// Remove all cached entries.  Intended for test teardown / full reset.
  void clear() {
    std::unique_lock<std::shared_mutex> lk(m_mtx);
    m_entries.clear();
  }

 private:
  struct cache_entry {
    std::string                            endpoint;
    std::chrono::steady_clock::time_point  expires_at;
  };

  mutable std::shared_mutex                          m_mtx;
  std::unordered_map<std::string, cache_entry>       m_entries;
};

}  // namespace oai::nef::app
