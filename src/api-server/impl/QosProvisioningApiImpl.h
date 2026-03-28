/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#ifndef QOS_PROVISIONING_API_IMPL_H_
#define QOS_PROVISIONING_API_IMPL_H_

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

class QosProvisioningApiImpl {
 public:
  QosProvisioningApiImpl(
      std::shared_ptr<Pistache::Rest::Router> router, nef_app* nef_app_inst,
      std::string address);
  ~QosProvisioningApiImpl() {}
  void init();

 private:
  void setup_routes();
  void create_subscription(
      const Pistache::Rest::Request&, Pistache::Http::ResponseWriter);
  void delete_subscription(
      const Pistache::Rest::Request&, Pistache::Http::ResponseWriter);
  void get_subscription(
      const Pistache::Rest::Request&, Pistache::Http::ResponseWriter);

  std::shared_ptr<Pistache::Rest::Router> m_router;
  nef_app* m_nef_app;
  std::string m_address;
};

}  // namespace api
}  // namespace nef
}  // namespace oai

#endif  // QOS_PROVISIONING_API_IMPL_H_
