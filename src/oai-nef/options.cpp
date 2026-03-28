/*
 * Copyright (c) 2017 Sprint
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 */

#include <getopt.h>
#include <iostream>
#include <stdlib.h>
#include <unistd.h>

#include "options.hpp"

int Options::options                = 0;
std::string Options::m_libconfigcfg = {};
bool Options::m_log_rot_file_log    = false;
bool Options::m_log_stdout          = false;

void Options::help() {
  std::cout << std::endl
            << "Usage:  nef  [OPTIONS]..." << std::endl
            << "  -h, --help                   Print help and exit" << std::endl
            << "  -c, --libconfigcfg filename  Read the application "
               "configuration from this file."
            << std::endl
            << "  -o, --stdoutlog              Send the application logs to "
               "STDOUT fd."
            << std::endl
            << "  -r, --rotatelog              Send the application logs to "
               "local file (in current working directory)."
            << std::endl;
}

bool Options::parse(int argc, char** argv) {
  bool ret = true;
  ret      = parseInputOptions(argc, argv);
  ret &= validateOptions();
  return ret;
}

bool Options::validateOptions() {
  return (options & libconfigcfg);
}

bool Options::parseInputOptions(int argc, char** argv) {
  int c;
  int option_index = 0;
  bool result      = true;

  struct option long_options[] = {
      {"help", no_argument, NULL, 'h'},
      {"libconfigcfg", required_argument, NULL, 'f'},
      {"stdoutlog", no_argument, NULL, 'o'},
      {"rotatelog", no_argument, NULL, 'r'},
      {NULL, 0, NULL, 0}};

  while (1) {
    c = getopt_long(argc, argv, "horc:", long_options, &option_index);
    if (c == -1) break;

    switch (c) {
      case 'h': {
        help();
        exit(0);
        break;
      }
      case 'c': {
        m_libconfigcfg = optarg;
        options |= libconfigcfg;
        break;
      }
      case 'o': {
        m_log_stdout = true;
        options |= log_stdout;
        break;
      }
      case 'r': {
        m_log_rot_file_log = true;
        options |= log_rot_file_log;
        break;
      }
      case '?': {
        switch (optopt) {
          case 'c': {
            std::cout << "Option -c requires an argument" << std::endl;
            break;
          }
          case 'o': {
            std::cout << "Option -o does not require an argument" << std::endl;
            break;
          }
          case 'r': {
            std::cout << "Option -r does not require an argument" << std::endl;
            break;
          }
          default: {
            std::cout << "Unrecognized option [" << c << "]" << std::endl;
            break;
          }
        }
        result = false;
        break;
      }
      default: {
        std::cout << "Unrecognized option [" << c << "]" << std::endl;
        result = false;
      }
    }
  }
  return result;
}
