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
/// Record each app as it lands in UDR; if the batch then fails, execute()
/// deletes them back out again. A failed rollback delete is only reported
/// through the optional callback — it never throws or re-fails, so deciding
/// how loudly to log it is the caller's call.
class PfdRollbackTracker {
 public:
  void mark_committed(const std::string& app_id) {
    committed_apps_.push_back(app_id);
  }

  /// Delete every committed app, in the order they were written. delete_fn
  /// returns false to signal a failed delete; on_error, if given, is called
  /// for each of those. Returns the number of failures, so 0 means the
  /// rollback was clean.
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
