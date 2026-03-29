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

#ifndef FILE_NEF_SEEN
#define FILE_NEF_SEEN

#include <cstdint>
#include <string>

#define _unused(x) ((void)(x))

// NEF Service names (TS 29.522)
#define NEF_SERVICE_MONITORING_EVENT  "nnef-eventexposure"
#define NEF_SERVICE_TRAFFIC_INFLUENCE "nnef-trafficinfluence"
#define NEF_SERVICE_PFD_MANAGEMENT    "nnef-pfdmanagement"
#define NEF_SERVICE_BDT               "nnef-bdt"
#define NEF_SERVICE_QOS_MONITORING    "nnef-qosmonitoring"
#define NEF_SERVICE_ANALYTICS         "nnef-analyticsexposure"

// NEF service types enum
typedef enum nef_service_type_s {
  NEF_SERVICE_TYPE_MONITORING_EVENT  = 0,
  NEF_SERVICE_TYPE_TRAFFIC_INFLUENCE = 1,
  NEF_SERVICE_TYPE_PFD_MANAGEMENT    = 2,
  NEF_SERVICE_TYPE_BDT               = 3,
  NEF_SERVICE_TYPE_QOS_MONITORING    = 4,
  NEF_SERVICE_TYPE_ANALYTICS         = 5,
  NEF_SERVICE_TYPE_UNKNOWN           = 6
} nef_service_type_t;

// Monitoring event types (TS 29.522 §5.7)
typedef enum nef_monitoring_event_type_s {
  NEF_EVENT_LOSS_OF_CONNECTIVITY   = 0,
  NEF_EVENT_UE_REACHABILITY        = 1,
  NEF_EVENT_LOCATION_REPORTING     = 2,
  NEF_EVENT_CHANGE_OF_IMSI_IMEI    = 3,
  NEF_EVENT_ROAMING_STATUS         = 4,
  NEF_EVENT_COMMUNICATION_FAILURE  = 5,
  NEF_EVENT_AVAILABILITY_AFTER_DDN = 6,
  NEF_EVENT_UNKNOWN                = 99
} nef_monitoring_event_type_t;

// NF type enum (mirrors NRF nf_type_t, needed for southbound mapping)
typedef enum nf_type_s {
  NF_TYPE_NRF     = 0,
  NF_TYPE_UDM     = 1,
  NF_TYPE_AMF     = 2,
  NF_TYPE_SMF     = 3,
  NF_TYPE_AUSF    = 4,
  NF_TYPE_NEF     = 5,
  NF_TYPE_PCF     = 6,
  NF_TYPE_SMSF    = 7,
  NF_TYPE_NSSF    = 8,
  NF_TYPE_UDR     = 9,
  NF_TYPE_LMF     = 10,
  NF_TYPE_GMLC    = 11,
  NF_TYPE_5G_EIR  = 12,
  NF_TYPE_SEPP    = 13,
  NF_TYPE_UPF     = 14,
  NF_TYPE_N3IWF   = 15,
  NF_TYPE_AF      = 16,
  NF_TYPE_UDSF    = 17,
  NF_TYPE_BSF     = 18,
  NF_TYPE_CHF     = 19,
  NF_TYPE_NWDAF   = 20,
  NF_TYPE_UNKNOWN = 21
} nf_type_t;

#endif /* FILE_NEF_SEEN */
