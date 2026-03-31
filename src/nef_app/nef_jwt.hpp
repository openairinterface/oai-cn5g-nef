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
  /*
   * Generate a bearer token for an NF consumer requesting NEF services.
   * Mirrors nrf_jwt::generate_signature.
   * @param [const std::string &] af_consumer_id: AF / NF consumer identity
   * @param [const std::string &] scope: NEF service scope (e.g.
   *        "3gpp-monitoring-event")
   * @param [const std::string &] nf_type: consumer NF type string
   * @param [const std::string &] target_nf_type: producer NF type string
   * @param [const std::string &] nef_instance_id: NEF instance UUID
   * @param [std::string &] token: output – signed JWT token string
   * @return true on success
   */
  bool generate_token(
      const std::string& af_consumer_id, const std::string& scope,
      const std::string& nf_type, const std::string& target_nf_type,
      const std::string& nef_instance_id, std::string& token) const;

  /*
   * Validate a bearer token presented by an AF.
   * @param [const std::string &] bearer_token: JWT string (without "Bearer ")
   * @param [const std::string &] required_scope: expected scope claim value
   * @param [const std::string &] af_id: expected subject claim (AF identity)
   * @return true if the token is valid for the requested scope and AF
   */
  bool validate_af_token(
      const std::string& bearer_token,
      const std::string& required_scope,
      const std::string& af_id) const;

  /*
   * Extract the 'sub' (subject) claim from a bearer token WITHOUT validating
   * the signature.  Used by handlers where the AF identity is not present in
   * the URL path; the sub claim is then passed to validate_af_token().
   * @param [const std::string &] bearer_token: JWT string (without "Bearer ")
   * @param [std::string &] out_sub: output — value of the 'sub' claim
   * @return true if the token was parseable and the sub claim was present
   */
  bool extract_sub_claim(
      const std::string& bearer_token,
      std::string& out_sub) const;

  /*
   * Retrieve the HMAC secret for the given scope / NF pair.
   * @param [const std::string &] scope: NEF service scope
   * @param [const std::string &] nf_type: consumer NF type
   * @param [const std::string &] target_nf_type: producer NF type
   * @param [std::string &] key: output secret
   * @return true on success
   */
  bool get_secret_key(
      const std::string& scope, const std::string& nf_type,
      const std::string& target_nf_type, std::string& key) const;

  /*
   * Retrieve the HMAC secret by instance-id variant.
   * @param [const std::string &] scope
   * @param [const std::string &] target_nf_instance_id
   * @param [std::string &] key
   * @return true on success
   */
  bool get_secret_key(
      const std::string& scope,
      const std::string& target_nf_instance_id,
      std::string& key) const;
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* FILE_NEF_JWT_HPP_SEEN */
