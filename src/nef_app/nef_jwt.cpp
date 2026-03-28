/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License. You may obtain a copy of the
 * License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file nef_jwt.cpp
 \brief NEF JWT validation/generation — mirrors nrf_jwt.cpp
 \author  OAI
 \date 2024
 */

#include "nef_jwt.hpp"

#include <ctime>
#include <vector>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "logger.hpp"
#include "nef_config.hpp"
#include "nef_config_types.hpp"

using namespace oai::nef::app;

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

namespace {

static int b64url_index(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

static bool base64url_decode(const std::string& in, std::string& out) {
  out.clear();
  int val  = 0;
  int valb = -8;
  for (char c : in) {
    if (c == '=') break;
    const int idx = b64url_index(c);
    if (idx < 0) return false;
    val = (val << 6) + idx;
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return true;
}

static bool split_jwt(
    const std::string& token, std::string& header_b64, std::string& payload_b64,
    std::string& sig_b64) {
  const auto p1 = token.find('.');
  if (p1 == std::string::npos) return false;
  const auto p2 = token.find('.', p1 + 1);
  if (p2 == std::string::npos) return false;
  header_b64  = token.substr(0, p1);
  payload_b64 = token.substr(p1 + 1, p2 - p1 - 1);
  sig_b64     = token.substr(p2 + 1);
  return !header_b64.empty() && !payload_b64.empty() && !sig_b64.empty();
}

}  // namespace

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
          "JWT scope mismatch: expected '%s', got '%s'", required_scope.c_str(),
          scope_val.c_str());
      return false;
    }

    if (!payload.contains("sub") || !payload["sub"].is_string()) {
      Logger::nef_app().warn("JWT missing required sub claim");
      return false;
    }
    const auto sub_val = payload["sub"].get<std::string>();
    if (sub_val != af_id) {
      Logger::nef_app().warn(
          "JWT sub mismatch: expected '%s', got '%s'", af_id.c_str(),
          sub_val.c_str());
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
