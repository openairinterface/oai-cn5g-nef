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

/*! \file api_conversions.hpp
 \brief
 \author  Tien-Thinh NGUYEN
 \company Eurecom
 \date 2022
 \email: tien-thinh.nguyen@eurecom.fr
 */

#ifndef FILE_API_CONVERSIONS_HPP_SEEN
#define FILE_API_CONVERSIONS_HPP_SEEN

#include "nef.h"

// using namespace oai::nef::model;
// using namespace oai::nef::app;

namespace oai {
namespace nef {
namespace api_conv {

bool validate_uuid(const std::string& str);

}  // namespace api_conv
}  // namespace nef
}  // namespace oai

#endif /* FILE_API_CONVERSIONS_HPP_SEEN */
