/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#ifndef PFD_MANAGEMENT_API_IMPL_H_
#define PFD_MANAGEMENT_API_IMPL_H_

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

class PfdManagementApiImpl {
 public:
  PfdManagementApiImpl(
      std::shared_ptr<Pistache::Rest::Router> router, nef_app* nef_app_inst,
      std::string address);
  ~PfdManagementApiImpl() {}
  void init();

 private:
  void setup_routes();
  void put_pfd(const Pistache::Rest::Request&, Pistache::Http::ResponseWriter);
  void delete_pfd(
      const Pistache::Rest::Request&, Pistache::Http::ResponseWriter);
  void get_pfd(const Pistache::Rest::Request&, Pistache::Http::ResponseWriter);
  void create_transaction(
      const Pistache::Rest::Request&, Pistache::Http::ResponseWriter);
  void list_transactions(
      const Pistache::Rest::Request&, Pistache::Http::ResponseWriter);

  std::shared_ptr<Pistache::Rest::Router> m_router;
  nef_app* m_nef_app;
  std::string m_address;
};

}  // namespace api
}  // namespace nef
}  // namespace oai

#endif  // PFD_MANAGEMENT_API_IMPL_H_
