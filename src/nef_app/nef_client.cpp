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

/*! \file nef_client.cpp
 \brief
 \author  Tien-Thinh NGUYEN
 \company Eurecom
 \date 2022
 \email: Tien-Thinh.Nguyen@eurecom.fr
 */

#include "nef_client.hpp"

#include <curl/curl.h>
#include <pistache/http.h>
#include <pistache/mime.h>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "3gpp_29.500.h"
#include "logger.hpp"
#include "nef.h"
#include "nef_config.hpp"

using namespace Pistache::Http;
using namespace Pistache::Http::Mime;
using namespace oai::nef::app;
using json = nlohmann::json;

extern nef_client* nef_client_inst;
extern nef_config nef_cfg;

//------------------------------------------------------------------------------
// To read content of the response from NF
static std::size_t callback(
    const char* in, std::size_t size, std::size_t num, std::string* out) {
  const std::size_t totalBytes(size * num);
  out->append(in, totalBytes);
  return totalBytes;
}
/*
//------------------------------------------------------------------------------
nef_client::nef_client(nef_event& ev) : m_event_sub(ev) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl_multi = curl_multi_init();
  handles    = {};
  headers    = NULL;
  headers    = curl_slist_append(headers, "Accept: application/json");
  headers    = curl_slist_append(headers, "Content-Type: application/json");
  headers    = curl_slist_append(headers, "charsets: utf-8");
  // subscribe_task_curl();
}
*/

//------------------------------------------------------------------------------
nef_client::~nef_client() {
  Logger::nef_app().debug("Delete NEF Client instance...");
  // Remove handle, free memory
  for (auto h : handles) {
    curl_multi_remove_handle(curl_multi, h);
    curl_easy_cleanup(h);
  }

  handles.clear();
  curl_multi_cleanup(curl_multi);
  curl_global_cleanup();
  curl_slist_free_all(headers);

  // if (task_connection.connected()) task_connection.disconnect();
}
