/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef _NEF_SBI_HELPER_HPP
#define _NEF_SBI_HELPER_HPP

#include "sbi_helper.hpp"

namespace oai::nef::api {

/**
 * NEF SBI path constants — both northbound (TS 29.522) and
 * southbound (AMF/SMF/PCF/UDR service paths).
 */
class nef_sbi_helper : public oai::common::sbi::sbi_helper {
 public:
  // Northbound API base paths (TS 29.522)
  static inline const std::string NefMonitoringEventBase =
      "/3gpp-monitoring-event/";
  static inline const std::string NefTrafficInfluenceBase =
      "/3gpp-traffic-influence/";
  static inline const std::string NefPfdManagementBase =
      "/3gpp-pfd-management/";
  static inline const std::string NefBdtBase = "/3gpp-bdt/";
  static inline const std::string NefQosMonitoringBase =
      "/3gpp-as-session-with-qos/";
  static inline const std::string NefAnalyticsBase = "/3gpp-analyticsexposure/";
  static inline const std::string NefResourceSubscriptions = "subscriptions";
  static inline const std::string NefResourceTransactions  = "transactions";
  static inline const std::string NefResourceApplications  = "applications";
  static inline const std::string NefResourceFetch         = "fetch";
  static inline const std::string NefResourcePolicies      = "policies";
  static inline const std::string NefResourceBdtPolicies   = "bdtPolicies";
  static inline const std::string NefPathSubscriptions     = "/subscriptions";
  static inline const std::string NefPathTransactions      = "/transactions";
  static inline const std::string NefPathApplications      = "/applications";
  static inline const std::string NefPathFetch             = "/fetch";
  static inline const std::string NefPathPolicies          = "/policies";
  static inline const std::string NefPathBdtPolicies       = "/bdtPolicies";

  // Southbound service base paths
  // AMF: Namf_EventExposure (TS 29.518)
  static inline const std::string AmfEventExposureBase = "/namf-evts/";
  // SMF: Nsmf_EventExposure (TS 29.508)
  static inline const std::string SmfEventExposureBase =
      "/nsmf-event-exposure/";
  // PCF: Npcf_PolicyAuthorization (TS 29.514)
  static inline const std::string PcfPolicyAuthBase =
      "/npcf-policyauthorization/";
  // UDR: Nudr_DataRepository (TS 29.504)
  static inline const std::string UdrDataRepositoryBase = "/nudr-dr/";
  // PCF: Npcf_BDTPolicyControl (TS 29.554)
  static inline const std::string PcfBdtPolicyControlBase =
      "/npcf-bdtpolicycontrol/";

  // NEF inbound notification endpoint (NEF gives this to AMF/SMF/PCF)
  // The path is: NefNotifyBase + api_version + "/notify/" + nf_sub_id
  // e.g.  /nef-notify/v1/notify/amf-sub-abc123
  static inline const std::string NefNotifyBase       = "/nef-notify/";
  static inline const std::string NefNotifyPathNotify = "/notify";
  static inline const std::string NefHealthPath       = "/health";

  // NNEF PFD Management service paths
  static inline const std::string NnefEventExposureBase =
      "/nnef-eventexposure/";
  static inline const std::string NnefPfdManagementBase =
      "/nnef-pfdmanagement/";
  static inline const std::string NnefPfdManagementPathTransactions =
      "/transactions";
  static inline const std::string NnefPfdManagementPathApplications =
      "/applications";
  static inline const std::string NnefPfdManagementPathPartialPull =
      "/partial-pull";
  static inline const std::string NnefPfdManagementPathSubscriptions =
      "/subscriptions";
};

}  // namespace oai::nef::api

#endif /* _NEF_SBI_HELPER_HPP */
