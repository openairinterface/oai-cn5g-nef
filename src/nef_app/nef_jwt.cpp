/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_jwt.hpp"

#include <ctime>
#include <vector>
#include <stdexcept>

#include "nef_jwt_detail.hpp"  // pure helpers, exposed for unit testing

#include <nlohmann/json.hpp>

#include "logger.hpp"
#include "nef_config.hpp"
#include "nef_config_types.hpp"

using namespace oai::nef::app;
using namespace oai::nef::app::detail;  // base64url_decode, split_jwt, verify_hs256_signature

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

//------------------------------------------------------------------------------
bool nef_jwt::generate_token(
    const std::string& af_consumer_id, const std::string& scope,
    const std::string& nf_type, const std::string& target_nf_type,
    const std::string& nef_instance_id, std::string& token) const {
  (void) af_consumer_id;
  (void) scope;
  (void) nf_type;
  (void) target_nf_type;
  (void) nef_instance_id;
  token.clear();
  Logger::nef_app().warn(
      "JWT token generation is not available in this build configuration");
  return false;
}

//------------------------------------------------------------------------------
bool nef_jwt::validate_af_token(
    const std::string& bearer_token, const std::string& required_scope,
    const std::string& af_id) const {
  try {
    std::string key;
    // Ensure JWT is explicitly enabled by config.
    if (!get_secret_key(required_scope, af_id, key)) {
      Logger::nef_app().warn(
          "Failed to validate JWT token: secret key is unavailable");
      return false;
    }

    std::string header_b64;
    std::string payload_b64;
    std::string sig_b64;
    if (!split_jwt(bearer_token, header_b64, payload_b64, sig_b64)) {
      Logger::nef_app().warn("Invalid JWT format");
      return false;
    }

    // --- Algorithm check (header must declare HS256) ---
    std::string header_json;
    if (!base64url_decode(header_b64, header_json)) {
      Logger::nef_app().warn("Failed to decode JWT header");
      return false;
    }
    {
      auto header = nlohmann::json::parse(header_json);
      if (!header.contains("alg") || !header["alg"].is_string()) {
        Logger::nef_app().warn("JWT header missing 'alg' field");
        return false;
      }
      const auto alg = header["alg"].get<std::string>();
      if (alg != "HS256") {
        Logger::nef_app().warn(
            "JWT unsupported algorithm '%s'; only HS256 is accepted",
            alg.c_str());
        return false;
      }
    }

    // --- Cryptographic signature verification ---
    // The signing input is the raw ASCII: header_b64url + "." + payload_b64url
    const std::string signing_input = header_b64 + "." + payload_b64;

    std::string sig_bytes;
    if (!base64url_decode(sig_b64, sig_bytes)) {
      Logger::nef_app().warn("Failed to decode JWT signature");
      return false;
    }

    if (!verify_hs256_signature(signing_input, sig_bytes, key)) {
      Logger::nef_app().warn("JWT signature verification failed");
      return false;
    }

    // --- Payload claims ---
    std::string payload_json;
    if (!base64url_decode(payload_b64, payload_json)) {
      Logger::nef_app().warn("Failed to decode JWT payload");
      return false;
    }

    auto payload = nlohmann::json::parse(payload_json);

    if (!payload.contains("scope") || !payload["scope"].is_string()) {
      Logger::nef_app().warn("JWT missing required scope claim");
      return false;
    }
    const auto scope_val = payload["scope"].get<std::string>();
    if (scope_val != required_scope) {
      Logger::nef_app().warn(
          "JWT scope mismatch: expected '%s', got '%s'",
          required_scope.c_str(), scope_val.c_str());
      return false;
    }

    if (!payload.contains("sub") || !payload["sub"].is_string()) {
      Logger::nef_app().warn("JWT missing required sub claim");
      return false;
    }
    const auto sub_val = payload["sub"].get<std::string>();
    if (sub_val != af_id) {
      Logger::nef_app().warn(
          "JWT sub mismatch: expected '%s', got '%s'",
          af_id.c_str(), sub_val.c_str());
      return false;
    }

    if (payload.contains("exp") && payload["exp"].is_number_integer()) {
      const auto now = static_cast<long long>(std::time(nullptr));
      const auto exp = payload["exp"].get<long long>();
      if (exp < now) {
        Logger::nef_app().warn("JWT token expired");
        return false;
      }
    }

    return true;
  } catch (const std::exception& e) {
    Logger::nef_app().warn("JWT validation failed: %s", e.what());
    return false;
  }
}

//------------------------------------------------------------------------------
bool nef_jwt::extract_sub_claim(
    const std::string& bearer_token,
    std::string& out_sub) const {
  using namespace oai::nef::app::detail;
  std::string hdr, pld, sig;
  if (!split_jwt(bearer_token, hdr, pld, sig)) return false;
  std::string pld_json;
  if (!base64url_decode(pld, pld_json)) return false;
  try {
    auto payload = nlohmann::json::parse(pld_json);
    if (payload.contains("sub") && payload["sub"].is_string()) {
      out_sub = payload["sub"].get<std::string>();
      return !out_sub.empty();
    }
  } catch (...) {}
  return false;
}

//------------------------------------------------------------------------------
bool nef_jwt::get_secret_key(
    const std::string& /*scope*/, const std::string& /*nf_type*/,
    const std::string& /*target_nf_type*/, std::string& key) const {
  std::string secret = nef_config_inst->nef()->get_jwt_secret_key();
  if (secret.empty()) {
    Logger::nef_app().warn(
        "JWT secret key is not configured – JWT validation is disabled");
    return false;
  }
  key = secret;
  return true;
}

//------------------------------------------------------------------------------
bool nef_jwt::get_secret_key(
    const std::string& /*scope*/,
    const std::string& /*target_nf_instance_id*/,
    std::string& key) const {
  std::string secret = nef_config_inst->nef()->get_jwt_secret_key();
  if (secret.empty()) {
    Logger::nef_app().warn(
        "JWT secret key is not configured – JWT validation is disabled");
    return false;
  }
  key = secret;
  return true;
}
