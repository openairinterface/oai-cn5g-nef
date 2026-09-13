/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <cstddef>

/// Compile-time constants and helpers for HTTP/2 request body size limiting.
namespace nef_request_limits {

/// Maximum HTTP/2 request body size (1 MiB default).
static constexpr std::size_t MAX_REQUEST_BODY_BYTES = 1u * 1024u * 1024u;

/// Returns true if accumulating @p incoming bytes on top of @p current bytes
/// would exceed @p max, signalling that the request should be rejected 413.
inline bool is_body_too_large(
    std::size_t current, std::size_t incoming,
    std::size_t max = MAX_REQUEST_BODY_BYTES) noexcept {
  return (current + incoming) > max;
}

}  // namespace nef_request_limits
