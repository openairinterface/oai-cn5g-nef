/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#pragma once

#include <cstddef>

/// @file nef_request_limits.hpp
/// Compile-time constants and helpers for HTTP/2 request body size limiting.
/// Future task: make MAX_REQUEST_BODY_BYTES config-driven via nef_config yaml.
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
