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

  /*
   * Handle a request to create a subscription (Event Exposure)
   * @param [std::string &] sub_id: ID of the created subscription
   * @param [const NefEventExposureSubsc &] ev_sub: Requested subscription's
   * information
   * @param [NefEventExposureSubsc &] created_ev_sub: Created subscription's
   * information
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @param [ProblemDetails &] problem_details: Store details of the error
   * @return void
   */
  void handle_create_individual_subscription(
      std::string& sub_id, const NefEventExposureSubsc& ev_sub,
      NefEventExposureSubsc& created_ev_sub, const uint8_t http_version,
      int& http_code, ProblemDetails& problem_details);

  /*
   * Handle a request to delete a subscription (Event Exposure)
   * @param [std::string &] sub_id: ID of the created subscription
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @param [ProblemDetails &] problem_details: Store details of the error
   * @return void
   */
  void handle_remove_individual_subscription(
      const std::string& sub_id, const uint8_t http_version, int& http_code,
      ProblemDetails& problem_details);

  /*
   * Handle a request to get a subscription information (Event Exposure)
   * @param [std::string &] sub_id: ID of the created subscription
   * @param [NefEventExposureSubsc &] ev_sub: Subscription's information
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @param [ProblemDetails &] problem_details: Store details of the error
   * @return void
   */
  void handle_get_individual_subscription(
      std::string& sub_id, NefEventExposureSubsc& ev_sub,
      const uint8_t http_version, int& http_code,
      ProblemDetails& problem_details);

  /*
   * Handle a request to update a subscription information (Event Exposure)
   * @param [std::string &] sub_id: ID of the created subscription
   * @param [const NefEventExposureSubsc &] ev_sub: Requested subscription's
   * information
   * @param [NefEventExposureSubsc &] updated_ev_sub: Updated subscription's
   * information
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @param [ProblemDetails &] problem_details: Store details of the error
   * @return void
   */
  void handle_update_individual_subscription(
      std::string& sub_id, const NefEventExposureSubsc& ev_sub,
      NefEventExposureSubsc& updated_ev_sub, const uint8_t http_version,
      int& http_code, ProblemDetails& problem_details);

 private:
};
}  // namespace oai::nef::app
#include "nef_config.hpp"

#endif /* FILE_NEF_APP_HPP_SEEN */
