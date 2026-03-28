/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#include "AnalyticsApiImpl.h"

#include <nlohmann/json.hpp>

#include "logger.hpp"
#include "nef_config.hpp"

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

namespace oai {
namespace nef {
namespace api {

using namespace oai::nef::app;

AnalyticsApiImpl::AnalyticsApiImpl(
    std::shared_ptr<Pistache::Rest::Router> router, nef_app* nef_app_inst,
    std::string address)
    : m_router(router), m_nef_app(nef_app_inst), m_address(address) {}

void AnalyticsApiImpl::init() {
  setup_routes();
}

void AnalyticsApiImpl::setup_routes() {
  using namespace Pistache::Rest;
  // TS 29.522 §5.12 / TS 29.520 (3gpp-analyticsExposure)
  Routes::Post(
      *m_router, "/3gpp-analyticsExposure/v1/:afId/subscriptions",
      Routes::bind(&AnalyticsApiImpl::create_subscription, this));
  Routes::Delete(
      *m_router, "/3gpp-analyticsExposure/v1/:afId/subscriptions/:subId",
      Routes::bind(&AnalyticsApiImpl::delete_subscription, this));
  Routes::Get(
      *m_router, "/3gpp-analyticsExposure/v1/:afId/subscriptions/:subId",
      Routes::bind(&AnalyticsApiImpl::get_subscription, this));
}

void AnalyticsApiImpl::create_subscription(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string af_id = request.param(":afId").as<std::string>();
  Logger::nef_sbi().info(
      "POST analytics subscription for AF: %s", af_id.c_str());

  nlohmann::json body = {};
  try {
    body = nlohmann::json::parse(request.body());
  } catch (...) {
    response.send(Pistache::Http::Code::Bad_Request, "Invalid JSON body");
    return;
  }

  std::string analytics_sub_id;
  nlohmann::json resp_body;
  int http_code = 0;

  m_nef_app->handle_analytics_subscription_create(
      af_id, body, analytics_sub_id, resp_body, http_code, 1);

  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  if (http_code == 201) {
    std::string loc = m_address + "/3gpp-analyticsExposure/v1/" + af_id +
                      "/subscriptions/" + analytics_sub_id;
    response.headers().add<Pistache::Http::Header::Location>(loc);
  }
  response.send(Pistache::Http::Code(http_code), resp_body.dump());
}

void AnalyticsApiImpl::delete_subscription(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string af_id  = request.param(":afId").as<std::string>();
  std::string sub_id = request.param(":subId").as<std::string>();

  int http_code = 0;
  m_nef_app->handle_analytics_subscription_delete(af_id, sub_id, http_code, 1);
  response.send(Pistache::Http::Code(http_code));
}

void AnalyticsApiImpl::get_subscription(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  response.send(Pistache::Http::Code::Ok, "{}");
}

}  // namespace api
}  // namespace nef
}  // namespace oai
