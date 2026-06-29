/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * nef_handler_group_b_test.cpp — Group B handler migration tests (Phase 4B).
 *
 * Tests:
 *   GroupB_NrfRegistration_NfTypeField
 *   GroupB_NrfRegistration_Ipv4AddressField
 *   GroupB_AmfNotif_SupiField_Propagated
 *   GroupB_AmfNotif_AbsentField_NotPropagated
 *   GroupB_SmfNotif_QosMeasurement_Propagated
 *
 * PCF → T8 QoS notification mapper tests (Task 9):
 *   GroupC_PcfQosNotif_GuaranteedMapped
 *   GroupC_PcfQosNotif_NotGuaranteedMapped
 *   GroupC_PcfQosNotif_MissingEvNotifs_ReturnsFalse
 *   GroupC_PcfQosNotif_SuccessfulResourcesAllocation_Mapped
 *   GroupC_PcfQosNotif_UsageReport_AccumulatedUsagePropagated
 *
 * AppSessionId validation tests (Task 9.10):
 *   GroupC_AppSessionId_Valid
 *   GroupC_AppSessionId_Empty_Invalid
 *   GroupC_AppSessionId_DotDot_Invalid
 *   GroupC_AppSessionId_Slash_Invalid
 *   GroupC_AppSessionId_Space_Invalid
 */

#include <string>
#include <variant>

#include <gtest/gtest.h>
#include <rfl/json.hpp>

#include "nef_notification_mapper.hpp"

using oai::nef::app::nef_notification_mapper;
using json = nlohmann::json;

// Provided by test_logger_init.cpp (compiled with c++17) to avoid
// spdlog/fmt-consteval incompatibility in this c++20 translation unit.
extern "C" void nef_test_init_logger();

struct NefLoggerEnv : testing::Environment {
  void SetUp() override { nef_test_init_logger(); }
};
static testing::Environment* const g_nef_logger_env =
    testing::AddGlobalTestEnvironment(new NefLoggerEnv);

// ---------------------------------------------------------------------------
// Helper: parse JSON string into rfl::Generic
// ---------------------------------------------------------------------------
static rfl::Generic parse(const std::string& json_str) {
  auto r = rfl::json::read<rfl::Generic>(json_str);
  EXPECT_TRUE(static_cast<bool>(r))
      << "JSON parse failed: " << (r ? "" : r.error().what());
  return r.value();
}

// Helper: get a string field from a rfl::Generic::Object
static std::string get_str(
    const rfl::Generic::Object& obj, const std::string& key) {
  auto r = obj.get(key);
  if (!r) return "";
  if (auto* s = std::get_if<std::string>(&r.value().variant())) return *s;
  return "";
}

// Helper: check whether a key exists in a rfl::Generic::Object
static bool has_key(
    const rfl::Generic::Object& obj, const std::string& key) {
  return static_cast<bool>(obj.get(key));
}

// ---------------------------------------------------------------------------
// NRF Registration profile tests (validate rfl::Generic::Object usage)
// ---------------------------------------------------------------------------

// GroupB_NrfRegistration_NfTypeField
// Verify that rfl::Generic::Object correctly stores and serialises "nfType"
// (mirrors the profile-building pattern in nef_client::register_to_nrf).
TEST(GroupB, NrfRegistration_NfTypeField) {
  rfl::Generic::Object profile;
  profile["nfInstanceId"] = rfl::Generic(std::string("test-uuid-1234"));
  profile["nfType"]       = rfl::Generic(std::string("NEF"));
  profile["nfStatus"]     = rfl::Generic(std::string("REGISTERED"));

  // Verify round-trip through JSON serialisation
  const std::string json_str = rfl::json::write(rfl::Generic(std::move(profile)));
  auto r = rfl::json::read<rfl::Generic>(json_str);
  ASSERT_TRUE(static_cast<bool>(r)) << r.error().what();

  const auto* obj = std::get_if<rfl::Generic::Object>(&r.value().variant());
  ASSERT_NE(obj, nullptr);

  EXPECT_EQ(get_str(*obj, "nfType"), "NEF");
  EXPECT_EQ(get_str(*obj, "nfStatus"), "REGISTERED");
  EXPECT_EQ(get_str(*obj, "nfInstanceId"), "test-uuid-1234");
}

// GroupB_NrfRegistration_Ipv4AddressField
// Verify that rfl::Generic::Array correctly stores IPv4 addresses
// (mirrors the ipv4Addresses array construction in nef_client::register_to_nrf).
TEST(GroupB, NrfRegistration_Ipv4AddressField) {
  rfl::Generic::Array addrs;
  addrs.push_back(rfl::Generic(std::string("10.0.0.1")));
  addrs.push_back(rfl::Generic(std::string("192.168.1.100")));

  rfl::Generic::Object profile;
  profile["nfType"]        = rfl::Generic(std::string("NEF"));
  profile["ipv4Addresses"] = rfl::Generic(std::move(addrs));

  // Round-trip
  const std::string json_str =
      rfl::json::write(rfl::Generic(std::move(profile)));
  auto r = rfl::json::read<rfl::Generic>(json_str);
  ASSERT_TRUE(static_cast<bool>(r)) << r.error().what();

  const auto* obj = std::get_if<rfl::Generic::Object>(&r.value().variant());
  ASSERT_NE(obj, nullptr);

  auto addrs_r = obj->get("ipv4Addresses");
  ASSERT_TRUE(static_cast<bool>(addrs_r));
  const auto* arr =
      std::get_if<rfl::Generic::Array>(&addrs_r.value().variant());
  ASSERT_NE(arr, nullptr);
  ASSERT_EQ(arr->size(), 2u);

  const auto* first = std::get_if<std::string>(&(*arr)[0].variant());
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(*first, "10.0.0.1");

  const auto* second = std::get_if<std::string>(&(*arr)[1].variant());
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(*second, "192.168.1.100");
}

// ---------------------------------------------------------------------------
// AMF → T8 notification mapper tests
// ---------------------------------------------------------------------------

// GroupB_AmfNotif_SupiField_Propagated
// An AMF notification with a SUPI in the reportList entry must propagate
// that SUPI to the corresponding T8 monitoringEventReports entry.
TEST(GroupB, AmfNotif_SupiField_Propagated) {
  const json amf_notif = json::parse(R"({
    "subscriptionId": "amf-sub-42",
    "reportList": [
      {
        "type": "LOCATION_REPORT",
        "supi": "imsi-001010123456789",
        "state": { "area": "CELL_GLOBAL_ID" }
      }
    ]
  })");

  json t8_out;
  const bool ok = nef_notification_mapper::amf_to_monitoring_notification(
      amf_notif, t8_out, "nef-sub-99");
  ASSERT_TRUE(ok);

  EXPECT_EQ(t8_out["subscription"], "nef-sub-99");

  const json& reports = t8_out["monitoringEventReports"];
  ASSERT_TRUE(reports.is_array());
  ASSERT_EQ(reports.size(), 1u);

  // supi must be propagated
  EXPECT_EQ(reports[0]["supi"], "imsi-001010123456789");
  // monitoringType translated correctly
  EXPECT_EQ(reports[0]["monitoringType"], "LOCATION_REPORTING");
}

// GroupB_AmfNotif_AbsentField_NotPropagated
// An AMF report entry without a "gpsi" field must NOT produce a "gpsi" key
// in the corresponding T8 report entry.
TEST(GroupB, AmfNotif_AbsentField_NotPropagated) {
  const json amf_notif = json::parse(R"({
    "subscriptionId": "amf-sub-43",
    "reportList": [
      {
        "type": "UE_REACHABILITY_FOR_DATA",
        "supi": "imsi-001010000000001"
      }
    ]
  })");

  json t8_out;
  const bool ok = nef_notification_mapper::amf_to_monitoring_notification(
      amf_notif, t8_out, "nef-sub-100");
  ASSERT_TRUE(ok);

  const json& reports = t8_out["monitoringEventReports"];
  ASSERT_TRUE(reports.is_array());
  ASSERT_EQ(reports.size(), 1u);

  // "gpsi" was absent in input — must NOT appear in output
  EXPECT_FALSE(reports[0].contains("gpsi"));
  // "reachabilityForData" must be set (amf_type == "UE_REACHABILITY_FOR_DATA")
  ASSERT_TRUE(reports[0].contains("reachabilityForData"));
  const bool reach = reports[0]["reachabilityForData"];
  EXPECT_TRUE(reach);
}

// ---------------------------------------------------------------------------
// SMF → T8 notification mapper test
// ---------------------------------------------------------------------------

// GroupB_SmfNotif_QosMeasurement_Propagated
// An OAI-SMF QOS_MON event with qosNotifType=GUARANTEED and delay measurements
// must produce a T8 eventReport with event="QOS_GUARANTEED" and qosMonReports.
TEST(GroupB, SmfNotif_QosMeasurement_Propagated) {
  const json smf_notif = json::parse(R"({
    "notifId": "notif-001",
    "eventNotifs": [
      {
        "event": "QOS_MON",
        "qosNotifType": "GUARANTEED",
        "ulDelays": [5, 10],
        "dlDelays": [3, 7]
      }
    ]
  })");

  json t8_out;
  const bool ok = nef_notification_mapper::smf_to_qos_notification(
      smf_notif, t8_out, "nef-sub-77");
  ASSERT_TRUE(ok);

  EXPECT_EQ(t8_out["transaction"], "nef-sub-77");

  const json& reports = t8_out["eventReports"];
  ASSERT_TRUE(reports.is_array());
  ASSERT_EQ(reports.size(), 1u);

  // QOS_MON + GUARANTEED → QOS_GUARANTEED
  EXPECT_EQ(reports[0]["event"], "QOS_GUARANTEED");

  // ulDelays propagated into qosMonReports
  ASSERT_TRUE(reports[0].contains("qosMonReports"));
  const json& qmr = reports[0]["qosMonReports"];
  ASSERT_TRUE(qmr.is_array());
  ASSERT_FALSE(qmr.empty());
  EXPECT_TRUE(qmr[0].contains("ulDelays"));
}

// ---------------------------------------------------------------------------
// PCF → T8 QoS notification mapper tests (Task 9)
// ---------------------------------------------------------------------------

// Test 9.3 — QOS_NOTIF + GUARANTEED qncReport → QOS_GUARANTEED + appliedQosRef
TEST(GroupC, PcfQosNotif_GuaranteedMapped) {
  const json pcf_notif = json::parse(R"({
    "evNotifs": [{"event": "QOS_NOTIF"}],
    "qncReports": [{"notifType": "GUARANTEED", "refQosIndication": "QOS-REF-001"}]
  })");

  json t8_out;
  const bool ok = nef_notification_mapper::pcf_to_qos_notification(
      pcf_notif, t8_out, "nef-qos-sub-01");
  ASSERT_TRUE(ok);

  EXPECT_EQ(t8_out["transaction"], "nef-qos-sub-01");

  const json& reports = t8_out["eventReports"];
  ASSERT_TRUE(reports.is_array());
  ASSERT_EQ(reports.size(), 1u);

  EXPECT_EQ(reports[0]["event"], "QOS_GUARANTEED");
  EXPECT_EQ(reports[0]["appliedQosRef"], "QOS-REF-001");
}

// Test 9.3b — QOS_NOTIF + NOT_GUARANTEED qncReport → QOS_NOT_GUARANTEED
TEST(GroupC, PcfQosNotif_NotGuaranteedMapped) {
  const json pcf_notif = json::parse(R"({
    "evNotifs": [{"event": "QOS_NOTIF"}],
    "qncReports": [{"notifType": "NOT_GUARANTEED", "refQosIndication": "QOS-REF-002"}]
  })");

  json t8_out;
  const bool ok = nef_notification_mapper::pcf_to_qos_notification(
      pcf_notif, t8_out, "nef-qos-sub-02");
  ASSERT_TRUE(ok);

  EXPECT_EQ(t8_out["eventReports"][0]["event"], "QOS_NOT_GUARANTEED");
  EXPECT_EQ(t8_out["eventReports"][0]["appliedQosRef"], "QOS-REF-002");
}

// Test 9.4 — Missing evNotifs → returns false, output is problem-detail
TEST(GroupC, PcfQosNotif_MissingEvNotifs_ReturnsFalse) {
  const json pcf_notif = json::parse(R"({
    "qncReports": [{"notifType": "GUARANTEED"}]
  })");

  json t8_out;
  const bool ok = nef_notification_mapper::pcf_to_qos_notification(
      pcf_notif, t8_out, "nef-qos-sub-03");
  EXPECT_FALSE(ok);
  // Output must be an RFC 7807 problem detail (has "title" field)
  EXPECT_TRUE(t8_out.contains("title"));
}

// Test 9.4b — SUCCESSFUL_RESOURCES_ALLOCATION → mapped verbatim
TEST(GroupC, PcfQosNotif_SuccessfulResourcesAllocation_Mapped) {
  const json pcf_notif = json::parse(R"({
    "evNotifs": [{"event": "SUCCESSFUL_RESOURCES_ALLOCATION"}]
  })");

  json t8_out;
  const bool ok = nef_notification_mapper::pcf_to_qos_notification(
      pcf_notif, t8_out, "nef-qos-sub-04");
  ASSERT_TRUE(ok);

  EXPECT_EQ(t8_out["eventReports"][0]["event"], "SUCCESSFUL_RESOURCES_ALLOCATION");
}

// Test 9.5 — USAGE_REPORT + top-level usgRep → accumulatedUsage propagated
TEST(GroupC, PcfQosNotif_UsageReport_AccumulatedUsagePropagated) {
  const json pcf_notif = json::parse(R"({
    "evNotifs": [{"event": "USAGE_REPORT"}],
    "usgRep": {
      "duration": 3600,
      "totalVolume": 1073741824,
      "downlinkVolume": 536870912,
      "uplinkVolume": 536870912
    }
  })");

  json t8_out;
  const bool ok = nef_notification_mapper::pcf_to_qos_notification(
      pcf_notif, t8_out, "nef-qos-sub-05");
  ASSERT_TRUE(ok);

  EXPECT_EQ(t8_out["eventReports"][0]["event"], "USAGE_REPORT");

  const json& usage = t8_out["eventReports"][0]["accumulatedUsage"];
  ASSERT_TRUE(usage.is_object());
  EXPECT_EQ(usage["totalVolume"], 1073741824);
  EXPECT_EQ(usage["downlinkVolume"], 536870912);
  EXPECT_EQ(usage["uplinkVolume"], 536870912);
}

// ---------------------------------------------------------------------------
// AppSessionId validation tests (Task 9.10)
// Mirrors nef_app::is_valid_app_session_id without linking nef_app.cpp.
// ---------------------------------------------------------------------------

static bool valid_app_session_id(const std::string& id) {
  if (id.empty() || id.size() > 253) return false;
  if (id.find("..") != std::string::npos) return false;
  for (const unsigned char c : id) {
    if (c == '/' || c == '\\') return false;
    if (std::isspace(c) || std::iscntrl(c)) return false;
  }
  return true;
}

TEST(GroupC, AppSessionId_Valid) {
  EXPECT_TRUE(valid_app_session_id("as-12345"));
  EXPECT_TRUE(valid_app_session_id("app-session.01"));
  EXPECT_TRUE(valid_app_session_id("a"));
  EXPECT_TRUE(valid_app_session_id(std::string(253, 'x')));
}

TEST(GroupC, AppSessionId_Empty_Invalid) {
  EXPECT_FALSE(valid_app_session_id(""));
}

TEST(GroupC, AppSessionId_TooLong_Invalid) {
  EXPECT_FALSE(valid_app_session_id(std::string(254, 'x')));
}

TEST(GroupC, AppSessionId_DotDot_Invalid) {
  EXPECT_FALSE(valid_app_session_id(".."));
  EXPECT_FALSE(valid_app_session_id("a/../b"));
}

TEST(GroupC, AppSessionId_Slash_Invalid) {
  EXPECT_FALSE(valid_app_session_id("a/b"));
  EXPECT_FALSE(valid_app_session_id("a\\b"));
}

TEST(GroupC, AppSessionId_Space_Invalid) {
  EXPECT_FALSE(valid_app_session_id("a b"));
  EXPECT_FALSE(valid_app_session_id("a\tb"));
  EXPECT_FALSE(valid_app_session_id("a\nb"));
}
