/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_JWT_HPP_SEEN
#define FILE_NEF_JWT_HPP_SEEN

#include <string>

namespace oai {
namespace nef {
namespace app {

// HS256 bearer-token handling for the NEF northbound APIs. The shared secret
// comes from nef.security.jwt_secret in the YAML config; an empty secret turns
// JWT validation off.
class nef_jwt {
 public:
  // NOT IMPLEMENTED. Always clears token, logs a warning and returns false,
  // and nothing calls it today. The signature is kept for the eventual
  // counterpart of nrf_jwt::generate_signature: sign a bearer token for an NF
  // consumer, where scope is a NEF service scope such as
  // "3gpp-monitoring-event".
  bool generate_token(
      const std::string& af_consumer_id, const std::string& scope,
      const std::string& nf_type, const std::string& target_nf_type,
      const std::string& nef_instance_id, std::string& token) const;

  // Validate a token presented by an AF against an expected scope and
  // subject. Checks, in order: a configured secret, JWT shape, alg == HS256,
  // the HMAC signature, then the scope, sub and exp claims.
  //
  // bearer_token is the bare JWT, with the "Bearer " prefix already stripped.
  bool validate_af_token(
      const std::string& bearer_token, const std::string& required_scope,
      const std::string& af_id) const;

  // Read the 'sub' claim WITHOUT verifying the signature.
  //
  // Handlers whose URL path carries no AF identity use this to learn who is
  // claiming to call, then hand that subject to validate_af_token() for the
  // real check. On its own it proves nothing.
  bool extract_sub_claim(
      const std::string& bearer_token, std::string& out_sub) const;

  // HMAC secret for a scope / consumer / producer triple. Today every
  // overload returns the single configured nef.security.jwt_secret.
  bool get_secret_key(
      const std::string& scope, const std::string& nf_type,
      const std::string& target_nf_type, std::string& key) const;

  // Same, keyed by target NF instance-id instead of NF type.
  bool get_secret_key(
      const std::string& scope, const std::string& target_nf_instance_id,
      std::string& key) const;
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_JWT_HPP_SEEN */
