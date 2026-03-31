/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef NEF_JWT_DETAIL_HPP_SEEN
#define NEF_JWT_DETAIL_HPP_SEEN

#include <string>

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

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
  if (valb > -6)
    out.push_back(kChars[((val << 8) >> (valb + 8)) & 0x3F]);
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
  int val = 0;
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
    const std::string& token, std::string& header_b64,
    std::string& payload_b64, std::string& sig_b64) {
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
             hmac_buf,
             reinterpret_cast<const unsigned char*>(sig_bytes.data()),
             hmac_len) == 0;
}

/// Compute raw HMAC-SHA256 bytes (helper for building test tokens).
inline std::string hmac_sha256_raw(
    const std::string& key, const std::string& msg) {
  unsigned char hmac_buf[EVP_MAX_MD_SIZE];
  unsigned int hmac_len = 0;
  HMAC(
      EVP_sha256(),
      reinterpret_cast<const unsigned char*>(key.data()),
      static_cast<int>(key.size()),
      reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
      hmac_buf, &hmac_len);
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

}  // namespace detail
}  // namespace app
}  // namespace nef
}  // namespace oai

#endif  // NEF_JWT_DETAIL_HPP_SEEN
