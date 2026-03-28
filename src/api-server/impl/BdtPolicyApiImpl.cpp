/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#include "BdtPolicyApiImpl.h"

#include <nlohmann/json.hpp>

#include "logger.hpp"
#include "nef_config.hpp"

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

namespace oai {
namespace nef {
namespace api {

using namespace oai::nef::app;

BdtPolicyApiImpl::BdtPolicyApiImpl(
    std::shared_ptr<Pistache::Rest::Router> router, nef_app* nef_app_inst,
    std::string address)
    : m_router(router), m_nef_app(nef_app_inst), m_address(address) {}

void BdtPolicyApiImpl::init() {
  setup_routes();
}

void BdtPolicyApiImpl::setup_routes() {
  using namespace Pistache::Rest;
  // TS 29.522 §5.8  (3gpp-bdt)
  Routes::Post(
      *m_router, "/3gpp-bdt/v1/:afId/transactions",
      Routes::bind(&BdtPolicyApiImpl::create_policy, this));
  Routes::Patch(
      *m_router, "/3gpp-bdt/v1/:afId/transactions/:bdtId",
      Routes::bind(&BdtPolicyApiImpl::update_policy, this));
  Routes::Delete(
      *m_router, "/3gpp-bdt/v1/:afId/transactions/:bdtId",
      Routes::bind(&BdtPolicyApiImpl::delete_policy, this));
  Routes::Get(
      *m_router, "/3gpp-bdt/v1/:afId/transactions/:bdtId",
      Routes::bind(&BdtPolicyApiImpl::get_policy, this));
}

void BdtPolicyApiImpl::create_policy(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string af_id = request.param(":afId").as<std::string>();
  Logger::nef_sbi().info("POST BDT policy for AF: %s", af_id.c_str());

  nlohmann::json body = {};
  try {
    body = nlohmann::json::parse(request.body());
  } catch (...) {
    response.send(Pistache::Http::Code::Bad_Request, "Invalid JSON body");
    return;
  }

  std::string bdt_id;
  nlohmann::json resp_body;
  int http_code = 0;

  m_nef_app->handle_bdt_policy_create(
      af_id, body, bdt_id, resp_body, http_code, 1);

  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  if (http_code == 201) {
    std::string loc =
        m_address + "/3gpp-bdt/v1/" + af_id + "/transactions/" + bdt_id;
    response.headers().add<Pistache::Http::Header::Location>(loc);
  }
  response.send(Pistache::Http::Code(http_code), resp_body.dump());
}

void BdtPolicyApiImpl::update_policy(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string af_id  = request.param(":afId").as<std::string>();
  std::string bdt_id = request.param(":bdtId").as<std::string>();

  nlohmann::json body = {};
  try {
    body = nlohmann::json::parse(request.body());
  } catch (...) {
    response.send(Pistache::Http::Code::Bad_Request, "Invalid JSON body");
    return;
  }

  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_bdt_policy_update(
      af_id, bdt_id, body, resp_body, http_code, 1);

  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  response.send(Pistache::Http::Code(http_code), resp_body.dump());
}

void BdtPolicyApiImpl::delete_policy(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string af_id  = request.param(":afId").as<std::string>();
  std::string bdt_id = request.param(":bdtId").as<std::string>();

  int http_code = 0;
  m_nef_app->handle_bdt_policy_delete(af_id, bdt_id, http_code, 1);
  response.send(Pistache::Http::Code(http_code));
}

void BdtPolicyApiImpl::get_policy(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  // Basic 200 stub; full GET can query the bdt_sessions map
  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  response.send(Pistache::Http::Code::Ok, "{}");
}

}  // namespace api
}  // namespace nef
}  // namespace oai
