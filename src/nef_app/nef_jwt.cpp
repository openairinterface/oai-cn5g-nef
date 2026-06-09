/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_jwt.hpp"

#include <memory>

#include "nef_jwt_detail.hpp"  // JwtHeader, JwtPayload, jwt_validate_with_key_impl + pure helpers

#include "logger.hpp"
#include "nef_config.hpp"
#include "nef_config_types.hpp"

using namespace oai::nef::app;
using namespace oai::nef::app::detail;  // base64url_decode, split_jwt,
                                        // verify_hs256_signature,
                                        // jwt_validate_with_key_impl

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
  std::string key;
  if (!get_secret_key(required_scope, af_id, key)) {
    Logger::nef_app().warn(
        "Failed to validate JWT token: secret key is unavailable");
    return false;
  }
  return validate_with_key(bearer_token, required_scope, af_id, key);
}

//------------------------------------------------------------------------------
bool nef_jwt::validate_with_key(
    const std::string& bearer_token, const std::string& required_scope,
    const std::string& af_id, const std::string& key) const {
  const bool ok =
      jwt_validate_with_key_impl(bearer_token, required_scope, af_id, key);
  if (!ok) {
    Logger::nef_app().warn("JWT validate_with_key: validation rejected token");
  }
  return ok;
}

//------------------------------------------------------------------------------
bool nef_jwt::extract_sub_claim(
    const std::string& bearer_token, std::string& out_sub) const {
  using namespace oai::nef::app::detail;
  std::string hdr, pld, sig;
  if (!split_jwt(bearer_token, hdr, pld, sig)) return false;
  std::string pld_json;
  if (!base64url_decode(pld, pld_json)) return false;
  auto r_pld = rfl::json::read<JwtPayload>(pld_json);
  if (!r_pld) return false;
  const JwtPayload& payload = r_pld.value();
  if (payload.sub && !payload.sub->empty()) {
    out_sub = *payload.sub;
    return true;
  }
  return false;
}
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
    const std::string& /*scope*/, const std::string& /*target_nf_instance_id*/,
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
