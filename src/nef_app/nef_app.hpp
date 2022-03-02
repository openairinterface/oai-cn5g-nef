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

/*! \file nef_app.hpp
 \brief
 \author  Tien-Thinh NGUYEN
 \company Eurecom
 \date 2020
 \email: Tien-Thinh.Nguyen@eurecom.fr
 */

#ifndef FILE_NEF_APP_HPP_SEEN
#define FILE_NEF_APP_HPP_SEEN

#include <string>
#include "uint_generator.hpp"
#include "NefEventExposureSubsc.h"
#include "ProblemDetails.h"

using namespace oai::nef::model;

namespace oai::nef::app {

class nef_config;
class nef_app {
 public:
  explicit nef_app(const std::string& config_file);
  nef_app(nef_app const&) = delete;
  void operator=(nef_app const&) = delete;

  virtual ~nef_app();

  /*
   * Generate a random UUID for NEF instance
   * @param [void]
   * @return void
   */
  void generate_uuid();

  /*
   * Subscribe to events from other 5GC NFs (AMF/SMF/UDM,etc)
   * @param [void]
   * @return void
   */
  void subscribe_nfs_events();

  void handle_create_individual_subscription(
      const NefEventExposureSubsc& ev_sub,
      NefEventExposureSubsc& created_ev_sub, std::string& sub_id,
      const uint8_t http_version, int& http_code,
      ProblemDetails& problem_details);

 private:
};
}  // namespace oai::nef::app
#include "nef_config.hpp"

#endif /* FILE_NEF_APP_HPP_SEEN */
