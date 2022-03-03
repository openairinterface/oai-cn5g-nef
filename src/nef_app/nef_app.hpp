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
 \date 2022
 \email: Tien-Thinh.Nguyen@eurecom.fr
 */

#ifndef FILE_NEF_APP_HPP_SEEN
#define FILE_NEF_APP_HPP_SEEN

#include <string>
#include "uint_generator.hpp"
#include "NefEventExposureSubsc.h"
#include "NefEventExposureNotif.h"
#include "AmfEventNotification.h"
#include "NsmfEventExposureNotification.h"
#include "MonitoringReport.h"
#include "ProblemDetails.h"
#include "nef.h"
#include <shared_mutex>

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
   * Generate an unique ID for the new subscription
   * @param [const std::string &] sub_id: the generated ID
   * @return void
   */
  void generate_ev_subscription_id(std::string& sub_id);

  /*
   * Generate an unique ID for the new subscription
   * @param void
   * @return the generated ID
   */
  evsub_id_t generate_ev_subscription_id();

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
   * @param [nlohmann::json &] ev_sub: Subscription's information
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @param [ProblemDetails &] problem_details: Store details of the error
   * @return void
   */
  void handle_get_individual_subscription(
      const std::string& sub_id, nlohmann::json& ev_sub,
      const uint8_t http_version, int& http_code,
      ProblemDetails& problem_details);

  /*
   * Handle a request to update a subscription information (Event Exposure)
   * @param [std::string &] sub_id: ID of the created subscription
   * @param [const NefEventExposureSubsc &] ev_sub: Requested subscription's
   * information
   * @param [nlohmann::json &] updated_ev_sub: Updated subscription's
   * information or problem details (Store details of the error)
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @return void
   */
  void handle_update_individual_subscription(
      const std::string& sub_id, const NefEventExposureSubsc& ev_sub,
      nlohmann::json& updated_ev_sub, const uint8_t http_version,
      int& http_code);

  /*
   * Handle a NF event notification (from AMF/SMF/UDM, etc)
   * @param [const NefEventExposureNotif &] eventNotif: Notification data
   * @param [nlohmann::json &] response_data: response data
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @return void
   */
  void handle_nf_event_notification(
      const NefEventExposureNotif& eventNotif, nlohmann::json& response_data,
      const uint8_t http_version, int& http_code);

  /*
   * Handle an event notification from AMF
   * @param [const AmfEventNotification &] amfEventNotification: Notification
   * data
   * @param [nlohmann::json &] response_data: response data
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @return void
   */
  void handle_amf_event_notification(
      const AmfEventNotification& amfEventNotification,
      nlohmann::json& response_data, const uint8_t http_version,
      int& http_code);

  /*
   * Handle an event notification from SMF
   * @param [const NsmfEventExposureNotification &]
   * smfEventExposureNotification: Notification data
   * @param [nlohmann::json &] response_data: response data
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @return void
   */
  void handle_smf_event_notification(
      const NsmfEventExposureNotification& smfEventExposureNotification,
      nlohmann::json& response_data, const uint8_t http_version,
      int& http_code);

  /*
   * Handle an event notification from SMF
   * @param [const std::vector<MonitoringReport>&]
   * eventExposureNotif: Notification data
   * @param [nlohmann::json &] response_data: response data
   * @param [const uint8_t] http_version: HTTP version
   * @param [int &] http_code: HTTP code used to return to the service consumer
   * @return void
   */
  void handle_udm_event_notification(
      const std::vector<MonitoringReport>& eventExposureNotif,
      nlohmann::json& response_data, const uint8_t http_version,
      int& http_code);

  /*
   * Add a new individual subscription (Event Exposure) to the DB
   * @param [std::string &] sub_id: ID of the created subscription
   * @param [std::shared_ptr<NefEventExposureSubsc> &] ces: Pointer to the
   * created subscription
   * @return true if the subscription is created successfully, otherwise return
   * false
   */
  bool add_ee_subscription(
      const std::string& sub_id, std::shared_ptr<NefEventExposureSubsc> ces);

  /*
   * Remove an existing subscription (Event Exposure) from the DB
   * @param [std::string &] sub_id: ID of the created subscription
   * @return true if the subscription is removed successfully, otherwise return
   * false
   */
  bool remove_ee_subscription(const std::string& sub_id);

  /*
   * Get info of an existing subscription (Event Exposure) from the DB
   * @param [std::string &] sub_id: ID of the subscription
   * @param [nlohmann::json &] ev_sub: Store subscription info in JSON format
   * @return true if the subscription is existed, otherwise return
   * false
   */
  bool get_ee_subscription(const std::string& sub_id, nlohmann::json& ev_sub);

 private:
  util::uint_generator<uint32_t> evsub_id_generator;
  std::string nef_instance_id;  // NEF instance ID

  // NF's instance id <-> list of subscription IDs
  std::map<std::string, std::vector<std::string>> nef_subscriptions;
  mutable std::shared_mutex m_instance_id2nrf_profile;

  // Sub_id <->Subscription
  std::map<std::string, std::shared_ptr<NefEventExposureSubsc>>
      subscrition_id2nef_subscription;
  mutable std::shared_mutex m_subscription_id2nef_subscription;

  // Event Sub<->list of Subscriptions
  std::map<NefEvent_anyOf::eNefEvent_anyOf, std::set<std::string>>
      event_sub2subscriptions;
  mutable std::shared_mutex m_event_sub2subscriptions;
};
}  // namespace oai::nef::app
#include "nef_config.hpp"

#endif /* FILE_NEF_APP_HPP_SEEN */
