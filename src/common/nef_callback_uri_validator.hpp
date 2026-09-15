/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <arpa/inet.h>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <string>

/// Checks whether \p uri is acceptable as a notification callback address.
/// Returns an empty string when it is, and a human-readable error message
/// when it is not.
///
/// The address ranges below are blocked so that an AF cannot aim NEF's
/// notifications at NEF's own loopback, at a cloud metadata endpoint, or into
/// the operator's private network.
///
/// Rejected as malformed or unsupported:
///   - empty URI
///   - missing "://" separator
///   - scheme other than "http" or "https"
///   - empty host after the scheme
///   - unclosed '[' in an IPv6 literal
///   - a bracketed IPv6 literal that is not a valid address
///
/// Rejected by address range:
///   - IPv4 loopback      127.0.0.0/8
///   - IPv4 link-local    169.254.0.0/16
///   - IPv4 RFC 1918      10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16
///   - IPv6 loopback      ::1
///   - IPv6 link-local    fe80::/10
///   - IPv6 ULA           fc00::/7
///
/// A host that is not an IP literal is accepted without a DNS lookup.
inline std::string validate_callback_uri(const std::string& uri) {
  if (uri.empty()) {
    return "callback URI must not be empty";
  }

  // 1. Extract scheme
  const auto scheme_end = uri.find("://");
  if (scheme_end == std::string::npos) {
    return "callback URI is malformed: missing '://' separator";
  }

  std::string scheme = uri.substr(0, scheme_end);
  for (auto& c : scheme) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  if (scheme != "http" && scheme != "https") {
    return "callback URI scheme '" + scheme +
           "' is not allowed; only http and https are permitted";
  }

  // 2. Extract authority (host[:port])
  const auto authority_start = scheme_end + 3;  // skip "://"
  auto authority_end         = uri.find('/', authority_start);
  if (authority_end == std::string::npos) {
    authority_end = uri.find('?', authority_start);
  }
  if (authority_end == std::string::npos) {
    authority_end = uri.find('#', authority_start);
  }
  if (authority_end == std::string::npos) {
    authority_end = uri.size();
  }

  std::string authority =
      uri.substr(authority_start, authority_end - authority_start);

  // Strip userinfo (user[:pass]@host)
  const auto at_pos = authority.rfind('@');
  if (at_pos != std::string::npos) {
    authority = authority.substr(at_pos + 1);
  }

  if (authority.empty()) {
    return "callback URI is malformed: empty host";
  }

  // 3. Extract bare host
  std::string host;
  bool is_ipv6_literal = false;
  if (authority[0] == '[') {
    // IPv6 bracketed literal: [::1] or [::1]:port
    is_ipv6_literal        = true;
    const auto bracket_end = authority.find(']');
    if (bracket_end == std::string::npos) {
      return "callback URI is malformed: unclosed IPv6 bracket";
    }
    host = authority.substr(1, bracket_end - 1);
    // Strip URL-encoded zone ID (%25 is the percent-encoded '%')
    const auto pcnt = host.find("%25");
    if (pcnt != std::string::npos) host.replace(pcnt, 3, "%");
    // Strip zone ID (everything from '%' onward, e.g. fe80::1%eth0)
    const auto zone_pos = host.find('%');
    if (zone_pos != std::string::npos) host = host.substr(0, zone_pos);
  } else {
    // IPv4 or hostname — strip optional trailing ":port"
    const auto colon_pos = authority.rfind(':');
    host = (colon_pos != std::string::npos) ? authority.substr(0, colon_pos) :
                                              authority;
  }

  if (host.empty()) {
    return "callback URI is malformed: empty host after parsing";
  }

  // 4. Check IPv4 blocked ranges
  struct in_addr addr4 {};
  if (inet_pton(AF_INET, host.c_str(), &addr4) == 1) {
    const uint32_t ip = ntohl(addr4.s_addr);

    // 127.0.0.0/8  (loopback)
    if ((ip >> 24) == 127u) {
      return "callback URI host is a loopback address and is not allowed";
    }
    // 169.254.0.0/16  (link-local / cloud metadata endpoint)
    if ((ip >> 16) == 0xA9FEu) {
      return "callback URI host is a link-local address and is not allowed";
    }
    // 10.0.0.0/8  (RFC 1918)
    if ((ip >> 24) == 10u) {
      return "callback URI host is in an RFC 1918 private range and is not "
             "allowed";
    }
    // 172.16.0.0/12  (RFC 1918: 172.16.0.0 – 172.31.255.255)
    // Top 12 bits of 172.16.0.0 = 0xAC1.
    if ((ip >> 20) == 0xAC1u) {
      return "callback URI host is in an RFC 1918 private range and is not "
             "allowed";
    }
    // 192.168.0.0/16  (RFC 1918)
    if ((ip >> 16) == 0xC0A8u) {
      return "callback URI host is in an RFC 1918 private range and is not "
             "allowed";
    }

    return "";  // valid IPv4
  }

  // 5. Check IPv6 blocked ranges
  struct in6_addr addr6 {};
  if (inet_pton(AF_INET6, host.c_str(), &addr6) == 1) {
    // ::1  (loopback)
    static const uint8_t kLoopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(addr6.s6_addr, kLoopback6, 16) == 0) {
      return "callback URI host is a loopback address and is not allowed";
    }
    // fe80::/10  (link-local)
    // First octet 0xFE, top 2 bits of the second are 10 → FE80..FEBF.
    if (addr6.s6_addr[0] == 0xFEu && (addr6.s6_addr[1] & 0xC0u) == 0x80u) {
      return "callback URI host is a link-local address and is not allowed";
    }
    // fc00::/7  (IPv6 ULA: FC00..FDFF)
    // Top 7 bits of the first octet are 1111110 → first & 0xFE == 0xFC.
    if ((addr6.s6_addr[0] & 0xFEu) == 0xFCu) {
      return "callback URI host is in an IPv6 ULA range and is not allowed";
    }

    return "";  // valid IPv6
  }

  // A bracketed literal that inet_pton rejected is not a hostname either, so
  // it cannot fall through to the hostname case below.
  if (is_ipv6_literal) {
    return "callback URI host is not a valid IPv6 address";
  }

  // 6. Hostname (non-IP) — accepted, no DNS resolution attempted
  return "";
}
