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
#include "3gpp_conversions.hpp"
#include "AmfCreateEventSubscription.h"
#include "MonitoringType_anyOf.h"
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

  // Generate NF instance Id
  generate_uuid();
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
    Logger::nef_app().debug("Created subscription info: %s",
                            json_tmp.dump().c_str());
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
    Logger::nef_app().debug("Found subscription with Subscription ID %s ",
                            sub_id.c_str());
    Logger::nef_app().debug("Subscription info: %s ", ev_sub.dump().c_str());
    http_code = HTTP_STATUS_CODE_200_OK;
  } else {
    Logger::nef_app().debug("Subscription not found with Subscription ID %s",
                            sub_id.c_str());
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
    Logger::nef_app().debug("Updated subscription info: %s",
                            json_tmp.dump().c_str());
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
void nef_app::handle_nf_event_notification(
    const NefEventExposureNotif& eventNotif, nlohmann::json& response_data,
    const uint8_t http_version, int& http_code) {
  // Process the Notification data

  // Send Notification to the subscribed NFs
}

//------------------------------------------------------------------------------
void nef_app::handle_amf_event_notification(
    const AmfEventNotification& amfEventNotification,
    nlohmann::json& response_data, const uint8_t http_version, int& http_code) {
  // Process the Notification data

  // Send Notification to the subscribed NFs
}

//------------------------------------------------------------------------------
void nef_app::handle_smf_event_notification(
    const NsmfEventExposureNotification& smfEventExposureNotification,
    nlohmann::json& response_data, const uint8_t http_version, int& http_code) {
  // Process the Notification data

  // Send Notification to the subscribed NFs
}

//------------------------------------------------------------------------------
void nef_app::handle_udm_event_notification(
    const std::vector<MonitoringReport>& eventExposureNotif,
    nlohmann::json& response_data, const uint8_t http_version, int& http_code) {
  // Process the Notification data

  // Send Notification to the subscribed NFs
}

//------------------------------------------------------------------------------
void nef_app::handle_create_monitoring_event_subscription(
    std::string& sub_id, const MonitoringEventSubscription& ev_sub,
    MonitoringEventSubscription& created_ev_sub, const uint8_t http_version,
    int& http_code, ProblemDetails& problem_details) {
  Logger::nef_app().info(
      "Handle a request to create a Monitoring Event Subscription");
  nlohmann::json json_tmp = {};
  to_json(json_tmp, ev_sub);
  Logger::nef_app().debug("Subscription info: %s", json_tmp.dump().c_str());

  // Generate a subscription ID Id and store the corresponding information in a
  // map (subscription id, info)
  generate_ev_subscription_id(sub_id);

  // Subscribe to the corresponding NF (e.g., UDM/AMF/SMF) and receive the
  // notification
  int nf_http_code = 0;
  std::string nf_sub_id = {};

  subscribe_nf_events(ev_sub, sub_id, nf_sub_id, nf_http_code);

  if ((nf_http_code != 200) and (nf_http_code != 201) and
      (nf_http_code != 204)) {
    // TODO
    // Cannot subscribe to the corresponding NF
  }

  // Successfully subscribed to the corresponding NF
  created_ev_sub = ev_sub;
  // TODO: update created subscription with corresponding info

  std::shared_ptr<MonitoringEventSubscription> ces =
      std::make_shared<MonitoringEventSubscription>(created_ev_sub);

  if (add_ee_subscription(sub_id, ces)) {
    Logger::nef_app().debug(
        "Created a new subscription with Subscription ID %s", sub_id);

    to_json(json_tmp, created_ev_sub);
    Logger::nef_app().debug("Created subscription info: %s",
                            json_tmp.dump().c_str());
    http_code = HTTP_STATUS_CODE_201_CREATED;
    // TODO: immediate report is included (HTTP code 200)

  } else {
    Logger::nef_app().debug("Error when creating a new subscription!");
    // TODO: Set corresponding Code
    http_code = HTTP_STATUS_CODE_500_INTERNAL_SERVER_ERROR;
    // ProblemDetails
  }
  return;
}
//------------------------------------------------------------------------------
bool nef_app::add_ee_subscription(const std::string& sub_id,
                                  std::shared_ptr<NefEventExposureSubsc> ces) {
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
bool nef_app::get_ee_subscription(const std::string& sub_id,
                                  nlohmann::json& ev_sub) {
  std::shared_lock lock(m_subscription_id2nef_subscription);
  if (subscrition_id2nef_subscription.count(sub_id) > 0) {
    to_json(ev_sub, *subscrition_id2nef_subscription[sub_id]);
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
bool nef_app::add_ee_subscription(
    const std::string& sub_id,
    std::shared_ptr<MonitoringEventSubscription> ces) {
  std::unique_lock lock(m_subscription_id2nef_monitoring_subscription);
  subscrition_id2nef_monitoring_subscription[sub_id] = ces;

  // Store subscription per event
  MonitoringType monitoring_type = ces->getMonitoringType();
  MonitoringType_anyOf::eMonitoringType_anyOf value =
      monitoring_type.getEnumValue();
  event_sub2monitoring_subscriptions[value].insert(sub_id);

  return true;
}

//------------------------------------------------------------------------------
void nef_app::subscribe_nf_events(const MonitoringEventSubscription& ev_sub,
                                  const std::string& sub_id,
                                  std::string& nf_sub_id, int& http_code) {
  MonitoringType monitoring_type = ev_sub.getMonitoringType();
  MonitoringType_anyOf::eMonitoringType_anyOf event_type =
      monitoring_type.getEnumValue();

  switch (event_type) {
    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        LOSS_OF_CONNECTIVITY: {  // AMF
      subscribe_amf_events(sub_id, event_type, ev_sub, http_code);
      // TODO:
    } break;
    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        UE_REACHABILITY: {  // AMF/UDM
      // TODO:
    } break;
    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        LOCATION_REPORTING: {  // AMF, GMLC
      // TODO:
    } break;
    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        CHANGE_OF_IMSI_IMEI_ASSOCIATION: {  // UDM
      // TODO:
    } break;

    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        ROAMING_STATUS: {  // UDM
      // TODO:
    } break;

    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        COMMUNICATION_FAILURE: {  // AMF
      // TODO:
    } break;

    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        AVAILABILITY_AFTER_DDN_FAILURE: {  // AMF
      // TODO:
    } break;
    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        NUMBER_OF_UES_IN_AN_AREA: {  // AMF
      // TODO:
    } break;
    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        PDN_CONNECTIVITY_STATUS: {  // SMF

      // TODO:
    } break;
    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        DOWNLINK_DATA_DELIVERY_STATUS: {  // SMF
      // TODO:
    } break;

    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        API_SUPPORT_CAPABILITY: {  // SMF
      // TODO:
    } break;
    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        NUM_OF_REGD_UES: {
      // TODO:
    } break;

    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        NUM_OF_ESTD_PDU_SESSIONS: {
      // TODO:
    } break;

    case oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf::
        AREA_OF_INTEREST: {
      // TODO:
    } break;
    default: {
    }
  }
}

//------------------------------------------------------------------------------
void nef_app::subscribe_amf_events(
    const std::string& sub_id,
    oai::nef::model::MonitoringType_anyOf::eMonitoringType_anyOf& event_type,
    const MonitoringEventSubscription& ev_sub, int& http_code) {
  // Fill message content and send an Event Exposure msg to AMF
  oai::nef::model::AmfCreateEventSubscription create_ev_subscription = {};
  xgpp_conv::monitoring_event_to_amf_event(ev_sub, create_ev_subscription);
  AmfEventSubscription ev_subscription =
      create_ev_subscription.getSubscription();
  // SubsChangeNotifyUri
  ev_subscription.setSubsChangeNotifyUri(
      nef_cfg.get_event_exposure_subscription_url() + "/" + sub_id);
  // NfId
  ev_subscription.setNfId(nef_instance_id);

  create_ev_subscription.setSubscription(ev_subscription);
  nlohmann::json json_body = {};
  to_json(json_body, create_ev_subscription);
  std::string amf_uri = {};
  std::string response_data = {};
  std::string location = {};
  nef_client_inst->send_event_exposure_subscribe(
      json_body, amf_uri, response_data, http_code, location);
  return;
}
