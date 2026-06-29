/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * nef_handler_group_c_patch_test.cpp — Group C PATCH handler migration tests.
 *
 * Tests validate that nef_merge_patch() (used by all 4 PATCH handlers in
 * Phase 4C) correctly implements RFC 7396 semantics with rfl::Generic,
 * matching what the old nlohmann::json::merge_patch() would produce.
 *
 * Tests:
 *   GroupC_BdtPatch_NullDeletesKey
 *   GroupC_BdtPatch_AbsentKeyPreserved
 *   GroupC_TiPatch_ParityVsNlohmann
 *   GroupC_QosPatch_ParityVsNlohmann
 *   GroupC_PfdAppPatch_NestedMerge
 */

#include <optional>
#include <string>
#include <variant>

#include <gtest/gtest.h>
#include <rfl/json.hpp>
#include <nlohmann/json.hpp>

#include "nef_json_utils.hpp"

using oai::nef::app::nef_merge_patch;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static rfl::Generic parse(const std::string& json_str) {
  auto r = rfl::json::read<rfl::Generic>(json_str);
  EXPECT_TRUE(static_cast<bool>(r)) << "parse failed: " << json_str;
  return r.value();
}

static std::string to_str(const rfl::Generic& g) {
  return rfl::json::write(g);
}

// Apply the same patch via nlohmann::json::merge_patch to get reference output
static nlohmann::json nlohmann_merge_patch(
    const std::string& base_json, const std::string& patch_json) {
  nlohmann::json base  = nlohmann::json::parse(base_json);
  nlohmann::json patch = nlohmann::json::parse(patch_json);
  base.merge_patch(patch);
  return base;
}

// Check whether rfl::Generic result matches nlohmann reference
static void expect_parity(
    const std::string& base_json, const std::string& patch_json) {
  const rfl::Generic base  = parse(base_json);
  const rfl::Generic patch = parse(patch_json);
  const rfl::Generic result = nef_merge_patch(base, patch);

  nlohmann::json nlohmann_ref = nlohmann_merge_patch(base_json, patch_json);
  nlohmann::json rfl_as_nlohmann =
      nlohmann::json::parse(rfl::json::write(result));

  EXPECT_EQ(nlohmann_ref, rfl_as_nlohmann);
}

// ---------------------------------------------------------------------------
// BDT PATCH tests
// ---------------------------------------------------------------------------

// RFC 7396 §2: null values in the patch MUST remove the key from the base.
TEST(GroupC, BdtPatch_NullDeletesKey) {
  const std::string base = R"({
    "bdtRefId": "bdt-01",
    "transferPolicies": [{"ratingGroup": 1, "maxBitrateDl": "100 Mbps"}],
    "selectedPolicy": 0
  })";
  // Patch sets selectedPolicy to null → should be deleted
  const std::string patch = R"({"selectedPolicy": null})";

  const rfl::Generic result = nef_merge_patch(parse(base), parse(patch));
  const std::string result_str = to_str(result);
  nlohmann::json result_obj    = nlohmann::json::parse(result_str);

  // selectedPolicy must not be present after patch
  EXPECT_FALSE(result_obj.contains("selectedPolicy"))
      << "null patch should delete key; got: " << result_str;
  // bdtRefId must be preserved
  EXPECT_TRUE(result_obj.contains("bdtRefId"));
  EXPECT_EQ(result_obj["bdtRefId"].get<std::string>(), "bdt-01");
}

// RFC 7396 §2: keys absent from the patch must remain unchanged in the result.
TEST(GroupC, BdtPatch_AbsentKeyPreserved) {
  const std::string base = R"({
    "bdtRefId": "bdt-01",
    "transferPolicies": [{"ratingGroup": 1}],
    "selectedPolicy": 0,
    "suppFeat": "3"
  })";
  // Only update selectedPolicy
  const std::string patch = R"({"selectedPolicy": 1})";

  const rfl::Generic result  = nef_merge_patch(parse(base), parse(patch));
  nlohmann::json result_obj  = nlohmann::json::parse(to_str(result));

  EXPECT_EQ(result_obj["selectedPolicy"].get<int>(), 1);
  EXPECT_EQ(result_obj["suppFeat"].get<std::string>(), "3");
  EXPECT_EQ(result_obj["bdtRefId"].get<std::string>(), "bdt-01");
  // transferPolicies array must survive
  EXPECT_TRUE(result_obj.contains("transferPolicies"));
  EXPECT_TRUE(result_obj["transferPolicies"].is_array());
}

// ---------------------------------------------------------------------------
// TI PATCH tests
// ---------------------------------------------------------------------------

// Verify parity with nlohmann::merge_patch for a Traffic Influence payload.
TEST(GroupC, TiPatch_ParityVsNlohmann) {
  const std::string base = R"({
    "afTransId": "ti-session-42",
    "afServiceId": "svc-001",
    "dnn": "internet",
    "trafficFilters": [{"flowDescriptions": ["permit out ip from any to assigned"]}],
    "notifUri": "https://af.example.com/notify",
    "requestTestNotification": false,
    "suppFeat": "0f"
  })";
  const std::string patch = R"({
    "notifUri": "https://af-new.example.com/notify",
    "requestTestNotification": true,
    "trafficFilters": null
  })";

  expect_parity(base, patch);
}

// ---------------------------------------------------------------------------
// QoS PATCH tests
// ---------------------------------------------------------------------------

// Verify parity with nlohmann::merge_patch for a QoS Subscription payload.
TEST(GroupC, QosPatch_ParityVsNlohmann) {
  const std::string base = R"({
    "subId": "qos-sub-001",
    "notifUri": "https://af.example.com/qos-notify",
    "ueIpv4Addr": "10.0.0.1",
    "snssai": {"sst": 1, "sd": "000001"},
    "qosReference": "qos-ref-a",
    "altQoSReferences": ["ref-b", "ref-c"],
    "events": [{"event": "QOS_GUARANTEED"}],
    "suppFeat": "01"
  })";
  const std::string patch = R"({
    "notifUri": "https://af2.example.com/notify",
    "altQoSReferences": null,
    "snssai": {"sst": 1, "sd": "000002"}
  })";

  expect_parity(base, patch);
}

// ---------------------------------------------------------------------------
// PFD App PATCH tests
// ---------------------------------------------------------------------------

// Verify that a nested merge within pfdDatas[appId] is applied correctly.
// The PATCH handler extracts the app entry, applies nef_merge_patch, writes
// back — this test verifies the inner merge logic.
TEST(GroupC, PfdAppPatch_NestedMerge) {
  // Simulate the per-app entry stored at pfdDatas[appId]
  const std::string base = R"({
    "appId": "app-001",
    "pfds": [
      {
        "pfdId": "pfd-1",
        "flowDescriptions": ["permit out ip from 198.51.100.0/24 to assigned"]
      }
    ],
    "cachingTime": 3600,
    "allowedDelay": 10
  })";
  // Patch: update cachingTime, delete allowedDelay, append a new pfd
  const std::string patch = R"({
    "cachingTime": 7200,
    "allowedDelay": null,
    "pfds": [
      {
        "pfdId": "pfd-2",
        "urls": ["https://cdn.example.com"]
      }
    ]
  })";

  const rfl::Generic result = nef_merge_patch(parse(base), parse(patch));
  nlohmann::json result_obj = nlohmann::json::parse(to_str(result));

  // allowedDelay must be removed (null patch)
  EXPECT_FALSE(result_obj.contains("allowedDelay"));
  // cachingTime must be updated
  EXPECT_EQ(result_obj["cachingTime"].get<int>(), 7200);
  // pfds must be replaced (array replacement in merge-patch)
  EXPECT_TRUE(result_obj["pfds"].is_array());
  EXPECT_EQ(result_obj["pfds"].size(), 1u);
  EXPECT_EQ(result_obj["pfds"][0]["pfdId"].get<std::string>(), "pfd-2");
  // appId must be preserved
  EXPECT_EQ(result_obj["appId"].get<std::string>(), "app-001");

  // Also verify parity with nlohmann reference
  expect_parity(base, patch);
}
