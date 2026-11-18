/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <cstddef>

/// Request body size limit for the HTTP/2 server, and the check that
/// enforces it.
namespace nef_request_limits {

/// Maximum HTTP/2 request body size (1 MiB default).
static constexpr std::size_t MAX_REQUEST_BODY_BYTES = 1u * 1024u * 1024u;

/// True when adding @p incoming bytes to the @p current total would go past
/// @p max. A true here means the request must be rejected with 413.
inline bool is_body_too_large(
    std::size_t current, std::size_t incoming,
    std::size_t max = MAX_REQUEST_BODY_BYTES) noexcept {
  return (current + incoming) > max;
}

}  // namespace nef_request_limits
