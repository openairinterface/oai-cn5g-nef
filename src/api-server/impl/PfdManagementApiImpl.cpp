/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#include "PfdManagementApiImpl.h"

#include <nlohmann/json.hpp>

#include "logger.hpp"
#include "nef_config.hpp"

extern std::unique_ptr<oai::config::nef::nef_config> nef_config_inst;

namespace oai {
namespace nef {
namespace api {

using namespace oai::nef::app;

PfdManagementApiImpl::PfdManagementApiImpl(
    std::shared_ptr<Pistache::Rest::Router> router, nef_app* nef_app_inst,
    std::string address)
    : m_router(router), m_nef_app(nef_app_inst), m_address(address) {}

void PfdManagementApiImpl::init() {
  setup_routes();
}

void PfdManagementApiImpl::setup_routes() {
  using namespace Pistache::Rest;
  // Per-application PFD CRUD  (TS 29.522 §5.5)
  Routes::Put(
      *m_router,
      "/3gpp-pfd-management/v1/:scsAsId/transactions/:transId/applications/"
      ":appId",
      Routes::bind(&PfdManagementApiImpl::put_pfd, this));
  Routes::Delete(
      *m_router,
      "/3gpp-pfd-management/v1/:scsAsId/transactions/:transId/applications/"
      ":appId",
      Routes::bind(&PfdManagementApiImpl::delete_pfd, this));
  Routes::Get(
      *m_router,
      "/3gpp-pfd-management/v1/:scsAsId/transactions/:transId/applications/"
      ":appId",
      Routes::bind(&PfdManagementApiImpl::get_pfd, this));
  // Transaction-level create/list
  Routes::Post(
      *m_router, "/3gpp-pfd-management/v1/:scsAsId/transactions",
      Routes::bind(&PfdManagementApiImpl::create_transaction, this));
  Routes::Get(
      *m_router, "/3gpp-pfd-management/v1/:scsAsId/transactions",
      Routes::bind(&PfdManagementApiImpl::list_transactions, this));
}

void PfdManagementApiImpl::put_pfd(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string app_id = request.param(":appId").as<std::string>();
  Logger::nef_sbi().info("PUT PFD for app: %s", app_id.c_str());

  nlohmann::json body = {};
  try {
    body = nlohmann::json::parse(request.body());
  } catch (...) {
    response.send(Pistache::Http::Code::Bad_Request, "Invalid JSON body");
    return;
  }

  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_pfd_create(app_id, body, resp_body, http_code, 1);

  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  response.send(Pistache::Http::Code(http_code), resp_body.dump());
}

void PfdManagementApiImpl::delete_pfd(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string app_id = request.param(":appId").as<std::string>();
  int http_code      = 0;
  m_nef_app->handle_pfd_delete(app_id, http_code, 1);
  response.send(Pistache::Http::Code(http_code));
}

void PfdManagementApiImpl::get_pfd(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  std::string app_id = request.param(":appId").as<std::string>();
  nlohmann::json resp_body;
  int http_code = 0;
  m_nef_app->handle_pfd_get(app_id, resp_body, http_code, 1);

  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  response.send(Pistache::Http::Code(http_code), resp_body.dump());
}

void PfdManagementApiImpl::create_transaction(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  // Transaction-level create: iterate pfdData entries and PUT each to UDR
  nlohmann::json body = {};
  try {
    body = nlohmann::json::parse(request.body());
  } catch (...) {
    response.send(Pistache::Http::Code::Bad_Request, "Invalid JSON body");
    return;
  }
  // For each appId in the body, call handle_pfd_create
  if (body.contains("pfdDatas") && body["pfdDatas"].is_object()) {
    for (auto& [app_id, pfd_data] : body["pfdDatas"].items()) {
      nlohmann::json dummy_resp;
      int dummy_code = 0;
      m_nef_app->handle_pfd_create(app_id, pfd_data, dummy_resp, dummy_code, 1);
    }
  }
  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  response.send(Pistache::Http::Code::Created, body.dump());
}

void PfdManagementApiImpl::list_transactions(
    const Pistache::Rest::Request& request,
    Pistache::Http::ResponseWriter response) {
  response.headers().add<Pistache::Http::Header::ContentType>(
      Pistache::Http::Mime::MediaType("application/json"));
  response.send(Pistache::Http::Code::Ok, "[]");
}

}  // namespace api
}  // namespace nef
}  // namespace oai
