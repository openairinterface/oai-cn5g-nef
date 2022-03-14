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

/*! \file nef_client.hpp
 \author  Tien-Thinh NGUYEN
 \company Eurecom
 \date 2022
 \email: Tien-Thinh.Nguyen@eurecom.fr
 */

#ifndef FILE_NEF_CLIENT_HPP_SEEN
#define FILE_NEF_CLIENT_HPP_SEEN

#include <curl/curl.h>

#include <boost/thread.hpp>
#include <boost/thread/future.hpp>
#include <map>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "uint_generator.hpp"

namespace oai::nef::app {

class nef_client {
 private:
  CURLM* curl_multi;
  std::vector<CURL*> handles;
  struct curl_slist* headers;

  mutable std::shared_mutex m_curl_handle_promises;
  std::map<uint32_t,
           boost::shared_ptr<boost::promise<std::pair<uint32_t, std::string>>>>
      curl_handle_promises;

  //  bs2::connection
  //      task_connection;  // connection for performing curl_multi every 1ms

 public:
  //  nef_client(nef_event& ev);
  nef_client(){};
  virtual ~nef_client();

  nef_client(nef_client const&) = delete;
  void operator=(nef_client const&) = delete;

  /*
   * Create Curl handle for multi curl
   * @param [const std::string &] uri: URI of the subscribed NF
   * @param [std::string &] response_data: response data
   * @param [uint32_t* ] promise_id: pointer to the promise id
   * @param [const std::string&] method: HTTP method
   * @return true if a handle was created successfully, otherwise return false
   */
  bool curl_create_handle(const std::string& uri, std::string& response_data,
                          uint32_t* promise_id, const std::string& method,
                          uint8_t http_version = 1);

  /*
   * Perform curl multi to actually process the available data
   * @param [uint64_t ms] ms: current time
   * @return void
   */
  void perform_curl_multi(uint64_t ms);

  /*
   * Release all the handles
   * @param void
   * @return void
   */
  void curl_release_handles();

  /*
   * Wait for the promise ready
   * @param [boost::shared_future<uint32_t>&] f: future
   * @return future value
   */
  std::pair<uint32_t, std::string> get_available_response(
      boost::shared_future<std::pair<uint32_t, std::string>>& f);

  /*
   * Store the promise
   * @param [uint32_t] pid: promise id
   * @param [boost::shared_ptr<boost::promise<uint32_t>>&] p: promise
   * @return void
   */
  void add_promise(
      uint32_t pid,
      boost::shared_ptr<boost::promise<std::pair<uint32_t, std::string>>>& p);

  /*
   * Remove the promise
   * @param [uint32_t] pid: promise id
   * @return void
   */
  void remove_promise(uint32_t id);

  /*
   * Set the value of the promise to make it ready
   * @param [uint32_t] pid: promise id
   * @param [uint32_t ] http_code: http response code
   * @return void
   */
  void trigger_process_response(uint32_t pid,
                                std::pair<uint32_t, std::string>& result);

  /*
   * Generate an unique value for promise id
   * @param void
   * @return generated promise id
   */
  static uint64_t generate_promise_id() {
    return util::uint_uid_generator<uint64_t>::get_instance().get_uid();
  }

  bool curl_create_handle(const std::string& uri, const std::string& data,
                          std::string& response_data, uint32_t* promise_id,
                          const std::string& method, uint8_t http_version = 1);

  void send_event_exposure_subscribe(nlohmann::json& json_body,
                                     std::string& amf_uri,
                                     std::string& response_data, int& http_code,
                                     std::string& location);
};
}  // namespace oai::nef::app
#endif /* FILE_NEF_CLIENT_HPP_SEEN */
