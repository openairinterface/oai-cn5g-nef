/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#ifndef TASK_MANAGER_H_
#define TASK_MANAGER_H_

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
  int sfd;
  bool terminate;
  bool terminated;
};

}  // namespace app
}  // namespace nef
}  // namespace oai

#endif /* TASK_MANAGER_H_ */
