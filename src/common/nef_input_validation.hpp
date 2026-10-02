/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace oai::nef::app {

// Every validator here returns an empty string when the value is acceptable,
// and a human-readable description of the problem when it is not.
//
// A field that is absent but not required always passes.

/// A string, no longer than max_len. Must also be non-empty when required;
/// an optional field that is present but empty passes.
inline std::string validate_string_field(
    const nlohmann::json& j, const std::string& field_name, bool required,
    std::size_t max_len = 256) {
  if (!j.contains(field_name)) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  if (!j.at(field_name).is_string()) {
    return field_name + ": must be a string";
  }
  const std::string val = j.at(field_name).get<std::string>();
  if (val.empty() && required) return field_name + ": must not be empty";
  if (val.size() > max_len)
    return field_name + ": exceeds maximum length " + std::to_string(max_len);
  return "";
}

/// A string drawn from a fixed set of allowed values.
inline std::string validate_enum_field(
    const nlohmann::json& j, const std::string& field_name,
    const std::unordered_set<std::string>& allowed_values, bool required) {
  if (!j.contains(field_name)) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  if (!j.at(field_name).is_string()) {
    return field_name + ": must be a string";
  }
  const std::string val = j.at(field_name).get<std::string>();
  if (allowed_values.find(val) == allowed_values.end()) {
    return field_name + ": invalid value '" + val + "'";
  }
  return "";
}

/// An integer within [min_val, max_val].
inline std::string validate_integer_field(
    const nlohmann::json& j, const std::string& field_name, bool required,
    int64_t min_val = INT64_MIN, int64_t max_val = INT64_MAX) {
  if (!j.contains(field_name)) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  if (!j.at(field_name).is_number_integer()) {
    return field_name + ": must be an integer";
  }
  const int64_t val = j.at(field_name).get<int64_t>();
  if (val < min_val || val > max_val) {
    return field_name + ": out of range [" + std::to_string(min_val) + ", " +
           std::to_string(max_val) + "]";
  }
  return "";
}

/// A JSON object.
inline std::string validate_object_field(
    const nlohmann::json& j, const std::string& field_name, bool required) {
  if (!j.contains(field_name)) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  if (!j.at(field_name).is_object()) {
    return field_name + ": must be an object";
  }
  return "";
}

/// A JSON array holding between min_size and max_size elements.
inline std::string validate_array_field(
    const nlohmann::json& j, const std::string& field_name, bool required,
    std::size_t min_size = 0, std::size_t max_size = 1000) {
  if (!j.contains(field_name)) {
    if (required) return field_name + ": required field missing";
    return "";
  }
  if (!j.at(field_name).is_array()) {
    return field_name + ": must be an array";
  }
  const std::size_t sz = j.at(field_name).size();
  if (sz < min_size) return field_name + ": array too small";
  if (sz > max_size) return field_name + ": array too large";
  return "";
}

/// A bare path or query parameter, rather than a field inside a JSON body.
inline std::string validate_string_param(
    const std::string& value, const std::string& param_name,
    std::size_t max_len = 256) {
  if (value.empty()) return param_name + ": must not be empty";
  if (value.size() > max_len)
    return param_name + ": exceeds maximum length " + std::to_string(max_len);
  return "";
}

/// The first non-empty error among the arguments, or "" if they all passed.
template<typename... Args>
inline std::string first_error(Args&&... args) {
  for (const std::string& e : {std::string(std::forward<Args>(args))...}) {
    if (!e.empty()) return e;
  }
  return "";
}

/// True when NEF has no way to authenticate anyone: no JWT secret and no AF
/// whitelist.
///
/// Callers deny the request on this, unless the operator has deliberately
/// turned on insecure_dev_mode.
inline bool is_auth_unconfigured(
    const std::string& jwt_secret, bool whitelist_empty) {
  return jwt_secret.empty() && whitelist_empty;
}

/// One NefEventSubs item (TS 29.591 §5.4.2): an object carrying a non-empty
/// "event" string.
inline std::string validate_nnef_event_subs_item(
    const nlohmann::json& item, std::size_t index = 0) {
  if (!item.is_object()) {
    return "eventsSubs[" + std::to_string(index) + "]: must be an object";
  }
  if (!item.contains("event") || !item.at("event").is_string() ||
      item.at("event").get<std::string>().empty()) {
    return "eventsSubs[" + std::to_string(index) +
           "].event: required non-empty string";
  }
  return "";
}

/// A NefEventExposureSubsc body (TS 29.591). eventsSubs must be a non-empty
/// array of valid items, and both notifUri and notifId must be present.
///
/// Whether notifUri is safe to call back is a separate question, answered in
/// the application layer.
///
/// No longer on the live path: handle_nnef_event_exposure_subscribe() now
/// does a typed NefEventExposureSubsc parse and validate() instead. Kept for
/// callers that only have the raw JSON.
inline std::string validate_nnef_event_exposure_subscription_body(
    const nlohmann::json& body) {
  auto err = validate_array_field(
      body, "eventsSubs", /*required=*/true,
      /*min_size=*/1);
  if (!err.empty()) return err;

  std::size_t idx = 0;
  for (const auto& item : body["eventsSubs"]) {
    err = validate_nnef_event_subs_item(item, idx);
    if (!err.empty()) return err;
    ++idx;
  }

  err = validate_string_field(body, "notifUri", /*required=*/true);
  if (!err.empty()) return err;

  err = validate_string_field(body, "notifId", /*required=*/true);
  if (!err.empty()) return err;

  return "";
}

}  // namespace oai::nef::app
