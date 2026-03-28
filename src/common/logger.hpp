/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.
 */

#pragma once

#include <cstdarg>
#include <stdexcept>
#include <vector>
#include "logger_base.hpp"

static const std::string ITTI    = "itti";
static const std::string NEF_APP = "nef_app";
static const std::string NEF_SBI = "nef_sbi";

class Logger : public oai::logger::logger_common {
 public:
  static void init(
      const std::string& name, const bool log_stdout, const bool log_rot_file) {
    oai::logger::logger_common(name, log_stdout, log_rot_file);
    oai::logger::logger_registry::register_logger(
        name, ITTI, log_stdout, log_rot_file);
    oai::logger::logger_registry::register_logger(
        name, NEF_APP, log_stdout, log_rot_file);
    oai::logger::logger_registry::register_logger(
        name, NEF_SBI, log_stdout, log_rot_file);
  }
  static void set_level(spdlog::level::level_enum level) {
    oai::logger::logger_registry::set_level(level);
  }
  static void set_lttng(bool isLttngActive) {
    oai::logger::logger_registry::set_lttng_is_active(isLttngActive);
  }
  static bool should_log(spdlog::level::level_enum level) {
    return oai::logger::logger_registry::should_log(level);
  }
  static const oai::logger::printf_logger& itti() {
    return oai::logger::logger_registry::get_logger(ITTI);
  }
  static const oai::logger::printf_logger& nef_app() {
    return oai::logger::logger_registry::get_logger(NEF_APP);
  }
  static const oai::logger::printf_logger& nef_sbi() {
    return oai::logger::logger_registry::get_logger(NEF_SBI);
  }
};
