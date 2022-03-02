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
 \date 2022
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
void nef_app::generate_ev_subscription_id(std::string& sub_id) {
  sub_id = std::to_string(evsub_id_generator.get_uid());
  Logger::nef_app().debug("Generated Subscription ID %s", sub_id.c_str());
}

//------------------------------------------------------------------------------
evsub_id_t nef_app::generate_ev_subscription_id() {
  return evsub_id_generator.get_uid();
}

//------------------------------------------------------------------------------
void nef_app::subscribe_nfs_events() {
  // TODO:
}

//------------------------------------------------------------------------------
void nef_app::handle_create_individual_subscription(
    std::string& sub_id, const NefEventExposureSubsc& ev_sub,
    NefEventExposureSubsc& created_ev_sub, const uint8_t http_version,
    int& http_code, ProblemDetails& problem_details) {
  Logger::nef_app().info(
      "Handle a request to Create an Individual Subscription (Event Exposure)");
  nlohmann::json json_tmp = {};
  to_json(json_tmp, ev_sub);
  Logger::nef_app().debug("Subscription info: %s", json_tmp.dump().c_str());

  // Generate a subscription ID Id and store the corresponding information in a
  // map (subscription id, info)
  generate_ev_subscription_id(sub_id);

  created_ev_sub = ev_sub;
  // TODO: update created subscription with corresponding info

  std::shared_ptr<NefEventExposureSubsc> ces =
      std::make_shared<NefEventExposureSubsc>(created_ev_sub);

  if (add_ee_subscription(sub_id, ces)) {
    Logger::nef_app().debug(
        "Created a new subscription with Subscription ID %s", sub_id);

    to_json(json_tmp, created_ev_sub);
    Logger::nef_app().debug(
        "Created subscription info: %s", json_tmp.dump().c_str());
    http_code = HTTP_STATUS_CODE_201_CREATED;

  } else {
    Logger::nef_app().debug("Error when creating a new subscription!");
    // TODO: Set corresponding Code
    http_code = HTTP_STATUS_CODE_500_INTERNAL_SERVER_ERROR;
    // ProblemDetails
  }
  return;
}

//------------------------------------------------------------------------------
void nef_app::handle_remove_individual_subscription(
    const std::string& sub_id, const uint8_t http_version, int& http_code,
    ProblemDetails& problem_details) {
  if (remove_ee_subscription(sub_id)) {
    Logger::nef_app().debug(
        "Successfully removed subscription with Subscription ID %s", sub_id);
    http_code = HTTP_STATUS_CODE_204_NO_CONTENT;

  } else {
    Logger::nef_app().debug("Error when deleting a new subscription!");
    // TODO: Set corresponding Code
    http_code = HTTP_STATUS_CODE_500_INTERNAL_SERVER_ERROR;
    // ProblemDetails
  }
  return;
}

//------------------------------------------------------------------------------
void nef_app::handle_get_individual_subscription(
    const std::string& sub_id, nlohmann::json& ev_sub,
    const uint8_t http_version, int& http_code,
    ProblemDetails& problem_details) {
  if (get_ee_subscription(sub_id, ev_sub)) {
    Logger::nef_app().debug(
        "Found subscription with Subscription ID %s ", sub_id.c_str());
    Logger::nef_app().debug("Subscription info: %s ", ev_sub.dump().c_str());
    http_code = HTTP_STATUS_CODE_200_OK;
  } else {
    Logger::nef_app().debug(
        "Subscription not found with Subscription ID %s", sub_id.c_str());
    http_code = HTTP_STATUS_CODE_404_NOT_FOUND;
    // TODO: ProblemDetails
  }
  return;
}

//------------------------------------------------------------------------------
void nef_app::handle_update_individual_subscription(
    const std::string& sub_id, const NefEventExposureSubsc& ev_sub,
    nlohmann::json& updated_ev_sub, const uint8_t http_version,
    int& http_code) {
  nlohmann::json json_tmp = {};
  // First remove the old subscription
  if (remove_ee_subscription(sub_id)) {
    Logger::nef_app().debug(
        "Successfully removed subscription with Subscription ID %s", sub_id);
  } else {
    Logger::nef_app().debug("Error when deleting a new subscription!");
    // TODO: Set corresponding Code
    http_code = HTTP_STATUS_CODE_500_INTERNAL_SERVER_ERROR;
    return;
  }
  // Then create a new one
  NefEventExposureSubsc created_ev_sub = ev_sub;
  // TODO: update created subscription with corresponding info

  std::shared_ptr<NefEventExposureSubsc> ces =
      std::make_shared<NefEventExposureSubsc>(created_ev_sub);

  if (add_ee_subscription(sub_id, ces)) {
    Logger::nef_app().debug(
        "Updated a new subscription with Subscription ID %s", sub_id);

    to_json(json_tmp, created_ev_sub);
    Logger::nef_app().debug(
        "Updated subscription info: %s", json_tmp.dump().c_str());
  } else {
    Logger::nef_app().debug("Error when updating a new subscription!");
    // TODO: Set corresponding Code
    http_code = HTTP_STATUS_CODE_500_INTERNAL_SERVER_ERROR;
    return;
  }

  http_code = HTTP_STATUS_CODE_200_OK;

  return;
}

//------------------------------------------------------------------------------
bool nef_app::add_ee_subscription(
    const std::string& sub_id, std::shared_ptr<NefEventExposureSubsc> ces) {
  std::unique_lock lock(m_subscription_id2nef_subscription);
  subscrition_id2nef_subscription[sub_id] = ces;

  // store subscription per event
  std::vector<NefEventSubs> event_subs = ces->getEventsSubs();
  for (auto e : event_subs) {
    NefEvent_anyOf::eNefEvent_anyOf value = e.getEvent().getEnumValue();
    event_sub2subscriptions[value].insert(sub_id);
  }
  return true;
}

//------------------------------------------------------------------------------
bool nef_app::remove_ee_subscription(const std::string& sub_id) {
  std::unique_lock lock(m_subscription_id2nef_subscription);
  if (subscrition_id2nef_subscription.count(sub_id) > 0) {
    // remove the list of subscriptions per event first
    std::shared_ptr<NefEventExposureSubsc> ces = {};
    ces = subscrition_id2nef_subscription.at(sub_id);
    std::vector<NefEventSubs> event_subs = ces->getEventsSubs();
    for (auto e : event_subs) {
      NefEvent_anyOf::eNefEvent_anyOf value = e.getEvent().getEnumValue();
      event_sub2subscriptions[value].erase(sub_id);
    }
    // then remove the subscription info
    subscrition_id2nef_subscription.erase(sub_id);
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
bool nef_app::get_ee_subscription(
    const std::string& sub_id, nlohmann::json& ev_sub) {
  std::shared_lock lock(m_subscription_id2nef_subscription);
  if (subscrition_id2nef_subscription.count(sub_id) > 0) {
    to_json(ev_sub, *subscrition_id2nef_subscription[sub_id]);
    return true;
  }
  return false;
}
