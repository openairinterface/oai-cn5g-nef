/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef NEF_JWT_DETAIL_HPP_SEEN
#define NEF_JWT_DETAIL_HPP_SEEN

#include <ctime>
#include <optional>
#include <string>

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

#include <rfl/json.hpp>

namespace oai {
namespace nef {
namespace app {
namespace detail {

/// Encode raw bytes as base64url (RFC 4648 §5, no padding).
inline std::string base64url_encode(const std::string& in) {
  static const char kChars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  int val = 0, valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(kChars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(kChars[((val << 8) >> (valb + 8)) & 0x3F]);
  return out;
}

/// Map a single base64url character to its 6-bit value; -1 on invalid input.
inline int b64url_index(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

/// Decode a base64url-encoded string (padding optional).
/// Returns false if the input contains illegal characters.
inline bool base64url_decode(const std::string& in, std::string& out) {
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

/// Split a JWT string into its three base64url-encoded components.
/// Returns false when the token does not contain exactly three non-empty parts.
inline bool split_jwt(
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

/// Verify an HS256 JWT signature using OpenSSL HMAC-SHA256.
/// signing_input = base64url(header) + "." + base64url(payload) (raw ASCII)
/// sig_bytes     = raw (decoded) signature bytes
/// secret        = shared HMAC secret
///
/// Uses CRYPTO_memcmp for constant-time comparison (prevents timing attacks).
inline bool verify_hs256_signature(
    const std::string& signing_input, const std::string& sig_bytes,
    const std::string& secret) {
  unsigned char hmac_buf[EVP_MAX_MD_SIZE];
  unsigned int hmac_len = 0;

  const unsigned char* key_data =
      reinterpret_cast<const unsigned char*>(secret.data());
  const unsigned char* msg_data =
      reinterpret_cast<const unsigned char*>(signing_input.data());

  if (!HMAC(
          EVP_sha256(), key_data, static_cast<int>(secret.size()), msg_data,
          signing_input.size(), hmac_buf, &hmac_len)) {
    return false;
  }

  if (hmac_len != sig_bytes.size()) return false;

  // Constant-time comparison to prevent timing-based side-channel attacks.
  return CRYPTO_memcmp(
             hmac_buf, reinterpret_cast<const unsigned char*>(sig_bytes.data()),
             hmac_len) == 0;
}

/// Compute raw HMAC-SHA256 bytes (helper for building test tokens).
inline std::string hmac_sha256_raw(
    const std::string& key, const std::string& msg) {
  unsigned char hmac_buf[EVP_MAX_MD_SIZE];
  unsigned int hmac_len = 0;
  HMAC(
      EVP_sha256(), reinterpret_cast<const unsigned char*>(key.data()),
      static_cast<int>(key.size()),
      reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), hmac_buf,
      &hmac_len);
  return std::string(reinterpret_cast<const char*>(hmac_buf), hmac_len);
}

/// Build a signed HS256 JWT from raw JSON header/payload strings.
/// Intended for use in tests only.
inline std::string make_test_jwt(
    const std::string& header_json, const std::string& payload_json,
    const std::string& secret) {
  const auto hdr  = base64url_encode(header_json);
  const auto pld  = base64url_encode(payload_json);
  const auto sign = hdr + "." + pld;
  return sign + "." + base64url_encode(hmac_sha256_raw(secret, sign));
}

// ---------------------------------------------------------------------------
// Typed structs for JWT header and payload parsing via rfl.
// These are in the detail namespace to allow use in unit tests without
// depending on nef_jwt.cpp (which has nef_config_inst dependencies).
// ---------------------------------------------------------------------------

/// JWT JOSE header. Unknown fields are silently ignored by rfl::json::read<>.
/// rfl::Rename is not needed: C++ field names (alg, typ) match JWT JSON keys
/// exactly.
struct JwtHeader {
  std::string alg;
  std::optional<std::string> typ;
};

/// JWT payload claims. Only the claims the NEF validates are listed.
/// Fields are optional so that validation code can emit specific error messages
/// for missing claims (fail-closed) rather than letting rfl deserialization
/// fail with a generic error. exp is absent in non-expiring tokens. rfl::Rename
/// is not needed: field names (sub, scope, exp) match JWT JSON keys exactly.
struct JwtPayload {
  std::optional<std::string> sub;
  std::optional<std::string> scope;
  std::optional<int64_t> exp;
};

/// Core JWT HS256 validation logic — no Logger calls, for testability.
/// Returns true if the token passes ALL checks:
///   - valid HS256 format (3 base64url parts)
///   - header parses as JwtHeader with alg == "HS256"
///   - signature verifies against `key`
///   - payload sub == af_id, scope == required_scope
///   - if exp is present, it must be in the future
inline bool jwt_validate_with_key_impl(
    const std::string& bearer_token, const std::string& required_scope,
    const std::string& af_id, const std::string& key) {
  std::string header_b64, payload_b64, sig_b64;
  if (!split_jwt(bearer_token, header_b64, payload_b64, sig_b64)) return false;

  // --- Algorithm check (BEFORE signature verification) ---
  std::string header_json;
  if (!base64url_decode(header_b64, header_json)) return false;
  {
    auto r_hdr = rfl::json::read<JwtHeader>(header_json);
    if (!r_hdr) return false;
    if (r_hdr.value().alg != "HS256") return false;
  }

  // --- Signature verification ---
  const std::string signing_input = header_b64 + "." + payload_b64;
  std::string sig_bytes;
  if (!base64url_decode(sig_b64, sig_bytes)) return false;
  if (!verify_hs256_signature(signing_input, sig_bytes, key)) return false;

  // --- Payload claims ---
  std::string payload_json;
  if (!base64url_decode(payload_b64, payload_json)) return false;
  auto r_pld = rfl::json::read<JwtPayload>(payload_json);
  if (!r_pld) return false;
  const JwtPayload& pld = r_pld.value();

  if (!pld.scope || pld.scope->empty() || *pld.scope != required_scope)
    return false;
  if (!pld.sub || pld.sub->empty() || *pld.sub != af_id) return false;
  if (pld.exp && *pld.exp < static_cast<int64_t>(std::time(nullptr)))
    return false;

  return true;
}

}  // namespace detail
}  // namespace app
}  // namespace nef
}  // namespace oai

#endif  // NEF_JWT_DETAIL_HPP_SEEN
