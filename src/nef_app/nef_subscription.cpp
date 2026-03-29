/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

#include "nef_subscription.hpp"

#include <boost/date_time/posix_time/time_formatters.hpp>

#include "logger.hpp"

using namespace oai::nef::app;

nef_subscription::nef_subscription(nef_event& ev) : m_event_sub(ev) {
  m_validity_time = boost::posix_time::from_iso_string("20991231T235959Z");
}

nef_subscription::~nef_subscription() {
  Logger::nef_app().debug("Delete NEF Subscription instance: %s",
                          m_af_sub_id.c_str());
  if (m_ev_connection.connected()) m_ev_connection.disconnect();
}

void nef_subscription::set_af_subscription_id(const std::string& sub_id) {
  m_af_sub_id = sub_id;
}
std::string nef_subscription::get_af_subscription_id() const {
  return m_af_sub_id;
}

void nef_subscription::set_nf_subscription_id(const std::string& nf_sub_id) {
  m_nf_sub_id = nf_sub_id;
}
std::string nef_subscription::get_nf_subscription_id() const {
  return m_nf_sub_id;
}

void nef_subscription::set_notification_uri(const std::string& uri) {
  m_notification_uri = uri;
}
std::string nef_subscription::get_notification_uri() const {
  return m_notification_uri;
}

void nef_subscription::set_service_type(nef_service_type_t svc) {
  m_service_type = svc;
}
nef_service_type_t nef_subscription::get_service_type() const {
  return m_service_type;
}

void nef_subscription::set_target_nf_type(nf_type_t nf_type) {
  m_target_nf_type = nf_type;
}
nf_type_t nef_subscription::get_target_nf_type() const {
  return m_target_nf_type;
}

void nef_subscription::set_validity_time(const boost::posix_time::ptime& t) {
  m_validity_time = t;
}
boost::posix_time::ptime nef_subscription::get_validity_time() const {
  return m_validity_time;
}

void nef_subscription::set_expire_time(
    const std::chrono::system_clock::time_point& t) {
  m_expire_time     = t;
  m_has_expire_time = true;
}

std::chrono::system_clock::time_point nef_subscription::get_expire_time() const {
  return m_expire_time;
}

bool nef_subscription::has_expire_time() const {
  return m_has_expire_time;
}

void nef_subscription::set_http_version(uint8_t ver) {
  m_http_version = ver;
}
uint8_t nef_subscription::get_http_version() const {
  return m_http_version;
}

void nef_subscription::set_scs_as_id(const std::string& id) {
  m_scs_as_id = id;
}
std::string nef_subscription::get_scs_as_id() const {
  return m_scs_as_id;
}

void nef_subscription::set_subscription_data(const nlohmann::json& data) {
  m_subscription_data = data;
}
nlohmann::json nef_subscription::get_subscription_data() const {
  return m_subscription_data;
}

void nef_subscription::display() const {
  Logger::nef_app().debug("NEF Subscription:");
  Logger::nef_app().debug("  AF Sub-ID  : %s", m_af_sub_id.c_str());
  Logger::nef_app().debug("  NF Sub-ID  : %s", m_nf_sub_id.c_str());
  Logger::nef_app().debug("  Notif URI  : %s", m_notification_uri.c_str());
  Logger::nef_app().debug("  SCS/AS ID  : %s", m_scs_as_id.c_str());
  Logger::nef_app().debug(
      "  Valid Until: %s",
      boost::posix_time::to_iso_string(m_validity_time).c_str());
}
