/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef FILE_NEF_JWT_HPP_SEEN
#define FILE_NEF_JWT_HPP_SEEN

#include <string>

namespace oai {
namespace nef {
namespace app {

class nef_jwt {
 public:
  // Sign a bearer token for an NF consumer, mirroring
  // nrf_jwt::generate_signature. scope is a NEF service scope such as
  // "3gpp-monitoring-event".
  bool generate_token(
      const std::string& af_consumer_id, const std::string& scope,
      const std::string& nf_type, const std::string& target_nf_type,
      const std::string& nef_instance_id, std::string& token) const;

  // Validate a token presented by an AF against an expected scope and
  // subject. bearer_token is the bare JWT, with the "Bearer " prefix stripped.
  bool validate_af_token(
      const std::string& bearer_token, const std::string& required_scope,
      const std::string& af_id) const;

  // Read the 'sub' claim WITHOUT verifying the signature. Handlers whose URL
  // path carries no AF identity use this to find out who is claiming to call,
  // then hand that subject to validate_af_token() for the real check.
  bool extract_sub_claim(
      const std::string& bearer_token, std::string& out_sub) const;

  // HMAC secret for a scope / consumer / producer triple.
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
