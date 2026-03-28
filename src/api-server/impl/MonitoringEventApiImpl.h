/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#ifndef MONITORING_EVENT_API_IMPL_H_
#define MONITORING_EVENT_API_IMPL_H_

#include <pistache/endpoint.h>
#include <pistache/http.h>
#include <pistache/router.h>
#include <memory>
#include <string>

#include "nef_app.hpp"

namespace oai {
namespace nef {
namespace api {

using namespace oai::nef::app;

class MonitoringEventApiImpl {
 public:
  MonitoringEventApiImpl(
      std::shared_ptr<Pistache::Rest::Router> router, nef_app* nef_app_inst,
      std::string address);
  ~MonitoringEventApiImpl() {}
  void init();

 private:
  void setup_routes();
  void subscribe(
      const Pistache::Rest::Request& request,
      Pistache::Http::ResponseWriter response);
  void unsubscribe(
      const Pistache::Rest::Request& request,
      Pistache::Http::ResponseWriter response);
  void get_subscription(
      const Pistache::Rest::Request& request,
      Pistache::Http::ResponseWriter response);

  std::shared_ptr<Pistache::Rest::Router> m_router;
  nef_app* m_nef_app;
  std::string m_address;
};

}  // namespace api
}  // namespace nef
}  // namespace oai

#endif /* MONITORING_EVENT_API_IMPL_H_ */
