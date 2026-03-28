/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#include "MonitoringEventApiImpl.h"

#include <nlohmann/json.hpp>

#include "3gpp_29.500.h"
#include "logger.hpp"
#include "nef_config.hpp"

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

namespace oai {
namespace nef {
namespace api {

using namespace oai::nef::app;

MonitoringEventApiImpl::MonitoringEventApiImpl(
    std::shared_ptr<Pistache::Rest::Router> router, nef_app* nef_app_inst,
    std::string address)
    : m_router(router), m_nef_app(nef_app_inst), m_address(address) {}

void MonitoringEventApiImpl::init() {
  setup_routes();
}

void MonitoringEventApiImpl::setup_routes() {
  using namespace Pistache::Rest;
  Routes::Post(
      *m_router, "/3gpp-monitoring-event/v1/:scsAsId/subscriptions",
      Routes::bind(&MonitoringEventApiImpl::subscribe, this));
  Routes::Delete(
      *m_router,
      "/3gpp-monitoring-event/v1/:scsAsId/subscriptions/:subscriptionId",
      Routes::bind(&MonitoringEventApiImpl::unsubscribe, this));
  Routes::Get(
      *m_router,
      "/3gpp-monitoring-event/v1/:scsAsId/subscriptions/:subscriptionId",
      Routes::bind(&MonitoringEventApiImpl::get_subscription, this));
}

void MonitoringEventApiImpl::subscribe(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string scs_as_id = request.param(":scsAsId").as<std::string>();
  Logger::nef_sbi().info(
      "POST monitoring event subscription for SCS/AS: %s", scs_as_id.c_str());

  nlohmann::json body = {};
  try {
    body = nlohmann::json::parse(request.body());
  } catch (...) {
    response.send(Pistache::Http::Code::Bad_Request, "Invalid JSON body");
    return;
  }

  std::string sub_id;
  nlohmann::json resp_body;
  int http_code = 0;

  m_nef_app->handle_monitoring_event_subscription_create(
      scs_as_id, body, sub_id, resp_body, http_code, 1);

  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  if (http_code == 201) {
    std::string loc = m_address + "/3gpp-monitoring-event/v1/" + scs_as_id +
                      "/subscriptions/" + sub_id;
    response.headers().add<Pistache::Http::Header::Location>(loc);
  }
  response.send(Pistache::Http::Code(http_code), resp_body.dump());
}

void MonitoringEventApiImpl::unsubscribe(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string scs_as_id = request.param(":scsAsId").as<std::string>();
  std::string sub_id    = request.param(":subscriptionId").as<std::string>();

  int http_code = 0;
  m_nef_app->handle_monitoring_event_subscription_delete(
      scs_as_id, sub_id, http_code, 1);

  response.send(Pistache::Http::Code(http_code));
}

void MonitoringEventApiImpl::get_subscription(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string scs_as_id = request.param(":scsAsId").as<std::string>();
  std::string sub_id    = request.param(":subscriptionId").as<std::string>();

  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_monitoring_event_subscription_get(
      scs_as_id, sub_id, resp_body, http_code, 1);

  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  response.send(Pistache::Http::Code(http_code), resp_body.dump());
}

}  // namespace api
}  // namespace nef
}  // namespace oai
