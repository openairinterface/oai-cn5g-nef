/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * nef_merge_patch_test.cpp — RFC 7396 merge-patch correctness tests (14 cases).
 */

#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <rfl/json.hpp>

#include "nef_json_utils.hpp"

using oai::nef::app::nef_merge_patch;

// ---------------------------------------------------------------------------
// Helper: load a file as a string
// ---------------------------------------------------------------------------
static std::string read_file(const std::string& path) {
  std::ifstream f(path);
  EXPECT_TRUE(f.is_open()) << "Cannot open fixture: " << path;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---------------------------------------------------------------------------
// Helper: apply merge-patch via reflect-cpp and return canonical JSON string
// (re-parsed through nlohmann for key-order-independent comparison)
// ---------------------------------------------------------------------------
static std::string rfl_merge(const std::string& base_str,
                              const std::string& patch_str) {
  auto r_base  = rfl::json::read<rfl::Generic>(base_str);
  auto r_patch = rfl::json::read<rfl::Generic>(patch_str);
  EXPECT_TRUE(static_cast<bool>(r_base))  << "rfl base parse failed";
  EXPECT_TRUE(static_cast<bool>(r_patch)) << "rfl patch parse failed";
  const std::string result =
      rfl::json::write(nef_merge_patch(r_base.value(), r_patch.value()));
  return nlohmann::json::parse(result).dump();
}

// Helper: apply merge-patch via nlohmann and return canonical JSON string
static std::string nlohmann_merge(const std::string& base_str,
                                  const std::string& patch_str) {
  auto j_base  = nlohmann::json::parse(base_str);
  auto j_patch = nlohmann::json::parse(patch_str);
  j_base.merge_patch(j_patch);
  return j_base.dump();
}

// ---------------------------------------------------------------------------
// Row 1: Non-object patch replaces base
// ---------------------------------------------------------------------------
TEST(MergePatch, NonObjectPatchReplacesBase) {
  const std::string result = rfl_merge(R"({"a":1})", R"("string")");
  EXPECT_EQ(result, nlohmann_merge(R"({"a":1})", R"("string")"));
  // Both must equal the string literal "string"
  EXPECT_EQ(nlohmann::json::parse(result), nlohmann::json("string"));
}

// ---------------------------------------------------------------------------
// Row 2: Non-object base overwritten by object patch
// ---------------------------------------------------------------------------
TEST(MergePatch, NonObjectBaseOverwrittenByObjectPatch) {
  const std::string result = rfl_merge(R"("scalar")", R"({"a":1})");
  EXPECT_EQ(result, nlohmann_merge(R"("scalar")", R"({"a":1})"));
  EXPECT_EQ(nlohmann::json::parse(result), nlohmann::json::parse(R"({"a":1})"));
}

// ---------------------------------------------------------------------------
// Row 3: null in patch deletes present key
// ---------------------------------------------------------------------------
TEST(MergePatch, NullDeletesKey) {
  const std::string result = rfl_merge(R"({"a":1,"b":2})", R"({"a":null})");
  EXPECT_EQ(result, nlohmann_merge(R"({"a":1,"b":2})", R"({"a":null})"));
  const auto j = nlohmann::json::parse(result);
  EXPECT_FALSE(j.contains("a")) << "key 'a' should have been deleted";
  EXPECT_EQ(j["b"], 2);
}

// ---------------------------------------------------------------------------
// Row 4: null for absent key is a no-op
// ---------------------------------------------------------------------------
TEST(MergePatch, NullOnAbsentKeyIsNoop) {
  const std::string result = rfl_merge(R"({"b":2})", R"({"a":null})");
  EXPECT_EQ(result, nlohmann_merge(R"({"b":2})", R"({"a":null})"));
  const auto j = nlohmann::json::parse(result);
  EXPECT_FALSE(j.contains("a"));
  EXPECT_EQ(j["b"], 2);
}

// ---------------------------------------------------------------------------
// Row 5: Array in patch replaces array in base
// ---------------------------------------------------------------------------
TEST(MergePatch, ArrayReplacement) {
  const std::string result = rfl_merge(R"({"a":[1,2]})", R"({"a":[3,4,5]})");
  EXPECT_EQ(result, nlohmann_merge(R"({"a":[1,2]})", R"({"a":[3,4,5]})"));
  const auto j = nlohmann::json::parse(result);
  EXPECT_EQ(j["a"], nlohmann::json::parse("[3,4,5]"));
}

// ---------------------------------------------------------------------------
// Row 6: Array patch on object-valued base key
// ---------------------------------------------------------------------------
TEST(MergePatch, ArrayPatchOnObjectValuedKey) {
  const std::string result = rfl_merge(R"({"a":{"x":1}})", R"({"a":[1,2]})");
  EXPECT_EQ(result, nlohmann_merge(R"({"a":{"x":1}})", R"({"a":[1,2]})"));
  const auto j = nlohmann::json::parse(result);
  EXPECT_TRUE(j["a"].is_array());
  EXPECT_EQ(j["a"], nlohmann::json::parse("[1,2]"));
}

// ---------------------------------------------------------------------------
// Row 7: Nested object recursion
// ---------------------------------------------------------------------------
TEST(MergePatch, NestedObjectRecurse) {
  const std::string base  = R"({"a":{"x":1,"y":2}})";
  const std::string patch = R"({"a":{"y":null,"z":3}})";
  const std::string result = rfl_merge(base, patch);
  EXPECT_EQ(result, nlohmann_merge(base, patch));
  const auto j = nlohmann::json::parse(result);
  EXPECT_EQ(j["a"]["x"], 1);
  EXPECT_FALSE(j["a"].contains("y")) << "key 'y' should have been deleted";
  EXPECT_EQ(j["a"]["z"], 3);
}

// ---------------------------------------------------------------------------
// Row 8: Deep 3-level recursion
// ---------------------------------------------------------------------------
TEST(MergePatch, DeepRecursion) {
  const std::string base  = R"({"a":{"b":{"c":1}}})";
  const std::string patch = R"({"a":{"b":{"d":2}}})";
  const std::string result = rfl_merge(base, patch);
  EXPECT_EQ(result, nlohmann_merge(base, patch));
  const auto j = nlohmann::json::parse(result);
  EXPECT_EQ(j["a"]["b"]["c"], 1);
  EXPECT_EQ(j["a"]["b"]["d"], 2);
}

// ---------------------------------------------------------------------------
// Row 9: Empty patch on object
// ---------------------------------------------------------------------------
TEST(MergePatch, EmptyPatchPreservesBase) {
  const std::string result = rfl_merge(R"({"a":1})", R"({})");
  EXPECT_EQ(result, nlohmann_merge(R"({"a":1})", R"({})"));
  EXPECT_EQ(nlohmann::json::parse(result)["a"], 1);
}

// ---------------------------------------------------------------------------
// Row 10: Both empty objects
// ---------------------------------------------------------------------------
TEST(MergePatch, BothEmptyObjects) {
  const std::string result = rfl_merge(R"({})", R"({})");
  EXPECT_EQ(result, nlohmann_merge(R"({})", R"({})"));
  EXPECT_TRUE(nlohmann::json::parse(result).empty());
}

// ---------------------------------------------------------------------------
// Rows 11–14: Real-payload parity tests
// ---------------------------------------------------------------------------

TEST(MergePatch, BdtPayloadParity) {
  const std::string base_str  = read_file(TEST_PAYLOAD_DIR "/bdt_base.json");
  const std::string patch_str = read_file(TEST_PAYLOAD_DIR "/bdt_patch.json");
  EXPECT_EQ(rfl_merge(base_str, patch_str), nlohmann_merge(base_str, patch_str));
}

TEST(MergePatch, TiPayloadParity) {
  const std::string base_str  = read_file(TEST_PAYLOAD_DIR "/ti_base.json");
  const std::string patch_str = read_file(TEST_PAYLOAD_DIR "/ti_patch.json");
  EXPECT_EQ(rfl_merge(base_str, patch_str), nlohmann_merge(base_str, patch_str));
}

TEST(MergePatch, QosPayloadParity) {
  const std::string base_str  = read_file(TEST_PAYLOAD_DIR "/qos_base.json");
  const std::string patch_str = read_file(TEST_PAYLOAD_DIR "/qos_patch.json");
  EXPECT_EQ(rfl_merge(base_str, patch_str), nlohmann_merge(base_str, patch_str));
}

TEST(MergePatch, PfdAppPayloadParity) {
  const std::string base_str  = read_file(TEST_PAYLOAD_DIR "/pfd_app_base.json");
  const std::string patch_str = read_file(TEST_PAYLOAD_DIR "/pfd_app_patch.json");
  EXPECT_EQ(rfl_merge(base_str, patch_str), nlohmann_merge(base_str, patch_str));
}
