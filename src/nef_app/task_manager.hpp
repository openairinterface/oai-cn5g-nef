/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

#ifndef TASK_MANAGER_H_
#define TASK_MANAGER_H_

#include "nef_event.hpp"

#include <linux/types.h>
#include <sys/timerfd.h>

using namespace oai::nef::app;

namespace oai {
namespace nef {
namespace app {

class nef_event;
class task_manager {
 public:
  explicit task_manager(nef_event& ev);
  ~task_manager();

  void manage_tasks();
  void run();

 private:
  void wait_for_cycle();

  nef_event& event_sub_;
  int        sfd;
  bool       terminate;
  bool       terminated;
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* TASK_MANAGER_H_ */
