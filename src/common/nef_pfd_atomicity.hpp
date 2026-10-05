/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

/// Best-effort compensating rollback for multi-app PFD batch writes.
///
/// Call mark_committed() for each app as it lands in UDR. If the batch then
/// fails, execute() deletes those apps back out again.
///
/// Atomicity is best-effort, not guaranteed. A rollback delete that itself
/// fails is not retried: execute() reports it through the optional callback
/// and the returned failure count, so UDR can be left holding an orphaned app.
/// The caller decides how loudly to log that. execute() catches nothing, so
/// delete_fn should signal failure by returning false, not by throwing.
///
/// The live async PFD PUT path does not use this tracker: nef_app's
/// pfd_put_rollback and pfd_rollback_step run the compensating DELETEs as
/// continuations, in reverse commit order.
class PfdRollbackTracker {
 public:
  void mark_committed(const std::string& app_id) {
    committed_apps_.push_back(app_id);
  }

  /// Delete every committed app, in the order they were written.
  ///
  /// delete_fn returns false to signal a failed delete, and on_error, if
  /// given, is called for each of those. Returns the number of failures, so 0
  /// means the rollback was clean.
  int execute(
      std::function<bool(const std::string&)> delete_fn,
      std::function<void(const std::string&)> on_error = nullptr) const {
    int failures = 0;
    for (const auto& app_id : committed_apps_) {
      if (!delete_fn(app_id)) {
        ++failures;
        if (on_error) on_error(app_id);
      }
    }
    return failures;
  }

  const std::vector<std::string>& committed() const { return committed_apps_; }
  bool has_committed() const { return !committed_apps_.empty(); }
  std::size_t committed_count() const { return committed_apps_.size(); }

 private:
  std::vector<std::string> committed_apps_;
};
