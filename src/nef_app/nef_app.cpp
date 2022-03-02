/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License. You may obtain a copy of the
 * License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file nef_app.cpp
 \brief
 \author  Tien-Thinh NGUYEN
 \company Eurecom
 \date 2020
 \email: Tien-Thinh.Nguyen@eurecom.fr
 */

#include "nef_app.hpp"

#include <unistd.h>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/date_time/posix_time/time_formatters.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>

#include "3gpp_29.500.h"
#include "3gpp_29.510.h"
#include "api_conversions.hpp"
#include "common_defs.h"
#include "logger.hpp"
#include "nef_client.hpp"
#include "nef_config.hpp"

using namespace oai::nef::app;
using namespace std::chrono;

extern nef_app* nef_app_inst;
extern nef_config nef_cfg;
nef_client* nef_client_inst = nullptr;

//------------------------------------------------------------------------------
nef_app::nef_app(const std::string& config_file) {
  Logger::nef_app().startup("Starting...");

  try {
    nef_client_inst = new nef_client();
  } catch (std::exception& e) {
    Logger::nef_app().error("Cannot create NEF_APP: %s", e.what());
    throw;
  }

  // subscribe to NFs' events
  // subscribe_nfs_events();

  Logger::nef_app().startup("Started");
}

//------------------------------------------------------------------------------
nef_app::~nef_app() {
  Logger::nef_app().debug("Delete NEF_APP instance...");
  // for (auto i : connections) {
  //   if (i.connected()) i.disconnect();
  // }

  if (nef_client_inst) delete nef_client_inst;
}

//------------------------------------------------------------------------------
void nef_app::generate_uuid() {
  // nef_instance_id = to_string(boost::uuids::random_generator()());
}

//------------------------------------------------------------------------------
void nef_app::subscribe_nfs_events() {
  // TODO:
}

//------------------------------------------------------------------------------
void nef_app::handle_create_individual_subscription(
    std::string& sub_id, const NefEventExposureSubsc& ev_sub,
    NefEventExposureSubsc& created_ev_sub, const uint8_t http_version,
    int& http_code, ProblemDetails& problem_details) {}

//------------------------------------------------------------------------------
void nef_app::handle_remove_individual_subscription(
    const std::string& sub_id, const uint8_t http_version, int& http_code,
    ProblemDetails& problem_details) {}

//------------------------------------------------------------------------------
void nef_app::handle_get_individual_subscription(
    std::string& sub_id, NefEventExposureSubsc& ev_sub,
    const uint8_t http_version, int& http_code,
    ProblemDetails& problem_details) {}

//------------------------------------------------------------------------------
void nef_app::handle_update_individual_subscription(
    std::string& sub_id, const NefEventExposureSubsc& ev_sub,
    NefEventExposureSubsc& updated_ev_sub, const uint8_t http_version,
    int& http_code, ProblemDetails& problem_details) {}
