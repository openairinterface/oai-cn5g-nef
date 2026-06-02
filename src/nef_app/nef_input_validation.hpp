/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>
#include <rfl/Generic.hpp>

namespace oai::nef::app {

/// Validates that a field, if required, is present and is a non-empty string
/// whose length does not exceed max_len.  Returns empty string on success.
inline std::string validate_string_field(
    const rfl::Generic::Object& j, const std::string& field_name, bool required,
    std::size_t max_len = 256) {
  const auto field = j.get(field_name);
  if (!field) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  const auto val_r = field.value().to_string();
  if (!val_r) {
    return field_name + ": must be a string";
  }
  const std::string& val = val_r.value();
  if (val.empty() && required) return field_name + ": must not be empty";
  if (val.size() > max_len)
    return field_name + ": exceeds maximum length " + std::to_string(max_len);
  return "";
}

/// Validates that a field, if present, is a string whose value is in the
/// allowed set.  If required and absent, returns an error.
inline std::string validate_enum_field(
    const rfl::Generic::Object& j, const std::string& field_name,
    const std::unordered_set<std::string>& allowed_values, bool required) {
  const auto field = j.get(field_name);
  if (!field) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  const auto val_r = field.value().to_string();
  if (!val_r) {
    return field_name + ": must be a string";
  }
  const std::string& val = val_r.value();
  if (allowed_values.find(val) == allowed_values.end()) {
    return field_name + ": invalid value '" + val + "'";
  }
  return "";
}

/// Validates that a field, if required, is present and is an integer within
/// [min_val, max_val].
inline std::string validate_integer_field(
    const rfl::Generic::Object& j, const std::string& field_name, bool required,
    int64_t min_val = INT64_MIN, int64_t max_val = INT64_MAX) {
  const auto field = j.get(field_name);
  if (!field) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  const auto val_r = field.value().to_int64();
  if (!val_r) {
    return field_name + ": must be an integer";
  }
  const int64_t val = val_r.value();
  if (val < min_val || val > max_val) {
    return field_name + ": out of range [" + std::to_string(min_val) + ", " +
           std::to_string(max_val) + "]";
  }
  return "";
}

/// Validates that a field, if required, is present and is a JSON object.
inline std::string validate_object_field(
    const rfl::Generic::Object& j, const std::string& field_name,
    bool required) {
  const auto field = j.get(field_name);
  if (!field) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  if (!std::get_if<rfl::Generic::Object>(&field.value().variant())) {
    return field_name + ": must be an object";
  }
  return "";
}

/// Validates that a field, if required, is present and is a JSON array whose
/// size is within [min_size, max_size].
inline std::string validate_array_field(
    const rfl::Generic::Object& j, const std::string& field_name, bool required,
    std::size_t min_size = 0, std::size_t max_size = 1000) {
  const auto field = j.get(field_name);
  if (!field) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  const auto* arr = std::get_if<rfl::Generic::Array>(&field.value().variant());
  if (!arr) {
    return field_name + ": must be an array";
  }
  const std::size_t sz = arr->size();
  if (sz < min_size) return field_name + ": array too small";
  if (sz > max_size) return field_name + ": array too large";
  return "";
}

/// Validates a plain string path/query parameter (not a JSON field).
/// Returns an error string on failure, empty string on success.
inline std::string validate_string_param(
    const std::string& value, const std::string& param_name,
    std::size_t max_len = 256) {
  if (value.empty()) return param_name + ": must not be empty";
  if (value.size() > max_len)
    return param_name + ": exceeds maximum length " + std::to_string(max_len);
  return "";
}

/// Returns the first non-empty error string from the provided arguments.
template<typename... Args>
inline std::string first_error(Args&&... args) {
  for (const std::string& e : {std::string(std::forward<Args>(args))...}) {
    if (!e.empty()) return e;
  }
  return "";
}

/// Returns true when neither a JWT secret nor an AF whitelist has been
/// configured — i.e., the NEF has no authentication mechanism in place.
/// In fail-closed mode (insecure_dev_mode=false, the default) callers MUST
/// deny the request.  In insecure_dev_mode=true the operator has explicitly
/// opted into unauthenticated access (development/test only).
inline bool is_auth_unconfigured(
    const std::string& jwt_secret, bool whitelist_empty) {
  return jwt_secret.empty() && whitelist_empty;
}

/// Validates a single NefEventSubs item (TS 29.591 §5.4.2).
/// Each item must be an object with a non-empty "event" string field.
/// Returns empty string on success, error description on failure.
inline std::string validate_nnef_event_subs_item(
    const rfl::Generic& item, std::size_t index = 0) {
  const auto* obj = std::get_if<rfl::Generic::Object>(&item.variant());
  if (!obj) {
    return "eventsSubs[" + std::to_string(index) + "]: must be an object";
  }
  const auto ev = obj->get("event");
  if (!ev) {
    return "eventsSubs[" + std::to_string(index) +
           "].event: required non-empty string";
  }
  const auto ev_str = ev.value().to_string();
  if (!ev_str || ev_str.value().empty()) {
    return "eventsSubs[" + std::to_string(index) +
           "].event: required non-empty string";
  }
  return "";
}

/// Validates a NefEventExposureSubsc request body (TS 29.591).
/// Checks: eventsSubs (required, non-empty array; each item has event),
///         notifUri (required, non-empty string),
///         notifId  (required, non-empty string).
/// SSRF / URI safety check is performed separately in the application layer.
/// Returns empty string on success, error description on failure.
inline std::string validate_nnef_event_exposure_subscription_body(
    const rfl::Generic::Object& body) {
  auto err = validate_array_field(
      body, "eventsSubs", /*required=*/true,
      /*min_size=*/1);
  if (!err.empty()) return err;

  const auto events = body.get("eventsSubs");
  const auto* arr = std::get_if<rfl::Generic::Array>(&events.value().variant());
  if (arr) {
    std::size_t idx = 0;
    for (const auto& item : *arr) {
      err = validate_nnef_event_subs_item(item, idx);
      if (!err.empty()) return err;
      ++idx;
    }
  }

  err = validate_string_field(body, "notifUri", /*required=*/true);
  if (!err.empty()) return err;

  err = validate_string_field(body, "notifId", /*required=*/true);
  if (!err.empty()) return err;

  return "";
}

}  // namespace oai::nef::app
