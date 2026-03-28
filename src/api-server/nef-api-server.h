/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

#ifndef FILE_NEF_API_SERVER_SEEN
#define FILE_NEF_API_SERVER_SEEN

#include "pistache/endpoint.h"
#include "pistache/http.h"
#include "pistache/router.h"

#ifdef __linux__
#include <signal.h>
#include <unistd.h>
#include <vector>
#endif

#include "MonitoringEventApiImpl.h"
#include "TrafficInfluenceApiImpl.h"
#include "PfdManagementApiImpl.h"
#include "BdtPolicyApiImpl.h"
#include "QosProvisioningApiImpl.h"
#include "AnalyticsApiImpl.h"
#include "nef_app.hpp"

using namespace oai::nef::api;
using namespace oai::nef::app;

class NEFApiServer {
 public:
  NEFApiServer(Pistache::Address address, nef_app* nef_app_inst)
      : m_httpEndpoint(std::make_shared<Pistache::Http::Endpoint>(address)) {
    m_router  = std::make_shared<Pistache::Rest::Router>();
    m_address = address.host() + ":" + address.port().toString();

    m_monitoringEventApiImpl = std::make_shared<MonitoringEventApiImpl>(
        m_router, nef_app_inst, m_address);
    m_trafficInfluenceApiImpl = std::make_shared<TrafficInfluenceApiImpl>(
        m_router, nef_app_inst, m_address);
    m_pfdManagementApiImpl = std::make_shared<PfdManagementApiImpl>(
        m_router, nef_app_inst, m_address);
    m_bdtPolicyApiImpl =
        std::make_shared<BdtPolicyApiImpl>(m_router, nef_app_inst, m_address);
    m_qosProvisioningApiImpl = std::make_shared<QosProvisioningApiImpl>(
        m_router, nef_app_inst, m_address);
    m_analyticsApiImpl =
        std::make_shared<AnalyticsApiImpl>(m_router, nef_app_inst, m_address);
  }

  void init(size_t thr = 1);
  void start();
  void shutdown();

 private:
  std::shared_ptr<Pistache::Http::Endpoint> m_httpEndpoint;
  std::shared_ptr<Pistache::Rest::Router> m_router;
  std::shared_ptr<MonitoringEventApiImpl> m_monitoringEventApiImpl;
  std::shared_ptr<TrafficInfluenceApiImpl> m_trafficInfluenceApiImpl;
  std::shared_ptr<PfdManagementApiImpl> m_pfdManagementApiImpl;
  std::shared_ptr<BdtPolicyApiImpl> m_bdtPolicyApiImpl;
  std::shared_ptr<QosProvisioningApiImpl> m_qosProvisioningApiImpl;
  std::shared_ptr<AnalyticsApiImpl> m_analyticsApiImpl;
  std::string m_address;
};

#endif /* FILE_NEF_API_SERVER_SEEN */
