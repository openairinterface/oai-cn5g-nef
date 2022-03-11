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

/*! \file 3gpp_conversions.cpp
 * \brief
 * \author Lionel Gauthier
 * \company Eurecom
 * \email: lionel.gauthier@eurecom.fr
 */
#include "3gpp_conversions.hpp"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "conversions.hpp"

using namespace oai::nef::model;
//------------------------------------------------------------------------------
bool xgpp_conv::monitoring_event_to_amf_event(
    const oai::nef::model::MonitoringEventSubscription& monitoring_event_sub,
    oai::nef::model::AmfCreateEventSubscription& amf_event_sub) {
  // AmfEventSubscription
  AmfEventSubscription ev_subscription;

  // AMF EventList
  std::vector<AmfEvent> amf_events;
  AmfEvent amf_event = {};
  AmfEventType amf_event_type = {};

  switch (monitoring_event_sub.getMonitoringType().getEnumValue()) {
    case MonitoringType_anyOf::eMonitoringType_anyOf::UE_REACHABILITY: {
      amf_event_type.setEnumValue(
          AmfEventType_anyOf::eAmfEventType_anyOf::REACHABILITY_REPORT);
    } break;
    default: {
      // TODO:
      return false;
    } break;
  }
  amf_event.setType(amf_event_type);
  amf_events.push_back(amf_event);
  ev_subscription.setEventList(amf_events);

  // EventNotifyUri (Consumer's URI (e.g., AF))
  ev_subscription.setEventNotifyUri(
      monitoring_event_sub.getNotificationDestination());
  // NotifyCorrelationId

  // NfId -  to be set later on NEF APP
  // SubsChangeNotifyUri - to be set later on NEF APP
  // SubsChangeNotifyUriIsSet;

  // SubsChangeNotifyCorrelationId;
  // SubsChangeNotifyCorrelationIdIsSet;

  // Supi;
  // SupiIsSet;

  // GroupId;
  // GroupIdIsSet;

  // Gpsi;
  // GpsiIsSet;

  // Pei;
  // PeiIsSet;

  // AnyUE;
  // AnyUEIsSet;

  // Options;
  // OptionsIsSet;

  // SupportedFeatures
  amf_event_sub.setSupportedFeatures(
      monitoring_event_sub.getSupportedFeatures());

  return true;
}
