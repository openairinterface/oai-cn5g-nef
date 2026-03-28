/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

#include "nef-api-server.h"

#include <vector>
#include <signal.h>

#include "logger.hpp"
#include "pistache/endpoint.h"
#include "pistache/http.h"
#include "pistache/router.h"

#define PISTACHE_SERVER_MAX_PAYLOAD 32768

void NEFApiServer::init(size_t thr) {
  auto opts = Pistache::Http::Endpoint::options()
                  .threads(static_cast<int>(thr))
                  .flags(Pistache::Tcp::Options::ReuseAddr)
                  .maxPayload(PISTACHE_SERVER_MAX_PAYLOAD);
  m_httpEndpoint->init(opts);

  m_monitoringEventApiImpl->init();
  m_trafficInfluenceApiImpl->init();
  m_pfdManagementApiImpl->init();
  m_bdtPolicyApiImpl->init();
  m_qosProvisioningApiImpl->init();
  m_analyticsApiImpl->init();
}

void NEFApiServer::start() {
  Logger::nef_sbi().info(
      "NEF HTTP/1.1 server listening on %s", m_address.c_str());
  m_httpEndpoint->setHandler(m_router->handler());
  m_httpEndpoint->serve();
}

void NEFApiServer::shutdown() {
  m_httpEndpoint->shutdown();
}
