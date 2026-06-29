/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * nef_jwt_test.cpp — JWT migration tests (Phase 5).
 *
 * Tests validate that the rfl-based JWT parsing (JwtHeader + JwtPayload
 * structs in nef_jwt_detail.hpp) behaves correctly.
 *
 * Tests call detail::jwt_validate_with_key_impl() directly — this avoids
 * the nef_config_inst dependency in nef_jwt.cpp.
 *
 * Blocking gate tests (must all pass for Phase 5 acceptance):
 *   Jwt_ValidTokenNoExp_Accepted
 *   Jwt_RS256Alg_Rejected
 *   Jwt_ExpiredToken_Rejected
 *   Jwt_WrongScope_Rejected
 *   Jwt_MalformedHeaderJson_FailClosed
 *
 * Additional regression tests:
 *   Jwt_ValidTokenWithExp_Accepted
 *   Jwt_ExtraClaimsPresent_Accepted
 *   Jwt_AlgAbsentFromHeader_Rejected
 *   Jwt_WrongSub_Rejected
 *   Jwt_MalformedPayloadJson_FailClosed
 */

#include <ctime>
#include <string>

#include <gtest/gtest.h>

#include "nef_jwt_detail.hpp"

using oai::nef::app::detail::jwt_validate_with_key_impl;
using oai::nef::app::detail::make_test_jwt;

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------
static const std::string kSecret   = "test-secret-key-for-unit-tests";
static const std::string kScope    = "3gpp-monitoring-event";
static const std::string kAfId     = "af-unit-test-01";
static const std::string kHdrHS256 = R"({"alg":"HS256","typ":"JWT"})";

static std::string make_payload(
    const std::string& sub, const std::string& scope,
    std::optional<long long> exp = std::nullopt,
    const std::string& extra = "") {
  std::string p = "{\"sub\":\"" + sub + "\",\"scope\":\"" + scope + "\"";
  if (exp) p += ",\"exp\":" + std::to_string(*exp);
  if (!extra.empty()) p += "," + extra;
  p += "}";
  return p;
}

// ---------------------------------------------------------------------------
// ---- Blocking gate tests (5) -----------------------------------------------
// ---------------------------------------------------------------------------

// Test 2 (blocking): Valid HS256 token with sub + scope, NO exp → Accepted
TEST(Jwt, ValidTokenNoExp_Accepted) {
  const std::string payload = make_payload(kAfId, kScope);
  const std::string token   = make_test_jwt(kHdrHS256, payload, kSecret);
  EXPECT_TRUE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}

// Test 4 (blocking): alg=RS256 → Rejected
TEST(Jwt, RS256Alg_Rejected) {
  const std::string hdr_rs256 = R"({"alg":"RS256","typ":"JWT"})";
  const std::string payload   = make_payload(kAfId, kScope);
  // Build token signed with HS256 key — but alg field says RS256
  const std::string token = make_test_jwt(hdr_rs256, payload, kSecret);
  EXPECT_FALSE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}

// Test 6 (blocking): exp present and in the past → Rejected
TEST(Jwt, ExpiredToken_Rejected) {
  const long long past_exp = static_cast<long long>(std::time(nullptr)) - 3600;
  const std::string payload = make_payload(kAfId, kScope, past_exp);
  const std::string token   = make_test_jwt(kHdrHS256, payload, kSecret);
  EXPECT_FALSE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}

// Test 7 (blocking): Wrong scope → Rejected
TEST(Jwt, WrongScope_Rejected) {
  const std::string payload = make_payload(kAfId, "wrong-scope");
  const std::string token   = make_test_jwt(kHdrHS256, payload, kSecret);
  EXPECT_FALSE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}

// Test 9 (blocking): Malformed header JSON → fail-closed (Rejected)
TEST(Jwt, MalformedHeaderJson_FailClosed) {
  // Header base64url-encodes to invalid JSON
  const std::string bad_hdr = "{alg:HS256,typ:JWT}";  // not valid JSON
  const std::string payload = make_payload(kAfId, kScope);
  const std::string token   = make_test_jwt(bad_hdr, payload, kSecret);
  EXPECT_FALSE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}

// ---------------------------------------------------------------------------
// ---- Additional regression tests (5) --------------------------------------
// ---------------------------------------------------------------------------

// Test 1 (regression): Valid HS256 token with sub + scope + exp (future)
TEST(Jwt, ValidTokenWithExp_Accepted) {
  const long long future_exp = static_cast<long long>(std::time(nullptr)) + 3600;
  const std::string payload  = make_payload(kAfId, kScope, future_exp);
  const std::string token    = make_test_jwt(kHdrHS256, payload, kSecret);
  EXPECT_TRUE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}

// Test 3 (regression): Extra claims in payload → silently ignored (Accepted)
TEST(Jwt, ExtraClaimsPresent_Accepted) {
  const std::string payload = make_payload(
      kAfId, kScope, std::nullopt,
      "\"iss\":\"oai-nef\",\"aud\":\"3gpp-monitoring\",\"custom\":42");
  const std::string token = make_test_jwt(kHdrHS256, payload, kSecret);
  EXPECT_TRUE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}

// Test 5 (regression): alg field absent → rfl::json::read<JwtHeader> fails
TEST(Jwt, AlgAbsentFromHeader_Rejected) {
  const std::string hdr_no_alg = R"({"typ":"JWT"})";
  const std::string payload    = make_payload(kAfId, kScope);
  const std::string token      = make_test_jwt(hdr_no_alg, payload, kSecret);
  EXPECT_FALSE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}

// Test 8 (regression): Wrong sub → Rejected
TEST(Jwt, WrongSub_Rejected) {
  const std::string payload = make_payload("wrong-af-id", kScope);
  const std::string token   = make_test_jwt(kHdrHS256, payload, kSecret);
  EXPECT_FALSE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}

// Test 10 (regression): Malformed payload JSON → fail-closed (Rejected)
TEST(Jwt, MalformedPayloadJson_FailClosed) {
  const std::string bad_payload = "{sub:af,scope:svc}";  // not valid JSON
  const std::string token = make_test_jwt(kHdrHS256, bad_payload, kSecret);
  EXPECT_FALSE(jwt_validate_with_key_impl(token, kScope, kAfId, kSecret));
}
