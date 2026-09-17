/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef _NEF_SBI_HELPER_HPP
#define _NEF_SBI_HELPER_HPP

#include "sbi_helper.hpp"

namespace oai::nef::api {

/**
 * NEF SBI path constants: northbound (TS 29.522) and southbound (the
 * AMF/SMF/PCF/UDR service paths). The NefResource* names are bare segments,
 * the NefPath* names the same segments with a leading '/'.
 */
class nef_sbi_helper : public oai::common::sbi::sbi_helper {
 public:
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
};

}  // namespace oai::nef::api

#endif /* _NEF_SBI_HELPER_HPP */
