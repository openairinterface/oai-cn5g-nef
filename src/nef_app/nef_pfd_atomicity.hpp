/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 */

/*! \file nef_pfd_atomicity.hpp
 * \brief PFD Transaction Atomicity / Rollback
 *
 * Header-only compensating rollback tracker for multi-app PFD batch writes.
 *
 * Usage pattern inside a PFD transaction PUT handler:
 *
 *   PfdRollbackTracker rollback;
 *   for (const auto& [app_id, app_body] : apps.items()) {
 *     if (!udr_put(app_id, app_body)) {
 *       rollback.execute([&](const std::string& id){ return udr_delete(id); },
 *                        [&](const std::string& id){ log_error(id); });
 *       return error_response;
 *     }
 *     rollback.mark_committed(app_id);
 *   }
 *   // All writes succeeded — commit local state now.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

/// Best-effort compensating rollback tracker for multi-app PFD batch writes.
///
/// Records successfully written app IDs during a batch loop.  On failure,
/// `execute()` calls a user-supplied delete callback for each committed app.
/// Rollback delete failures are reported via an optional callback but do NOT
/// re-fail or throw — the caller bears responsibility for logging at the
/// appropriate severity level.
class PfdRollbackTracker {
 public:
  /// Mark @p app_id as successfully written to UDR.
  void mark_committed(const std::string& app_id) {
    committed_apps_.push_back(app_id);
  }

  /// Execute best-effort compensating rollback.
  ///
  /// Iterates over all committed app IDs in insertion order and invokes
  /// @p delete_fn for each one.
  ///
  /// @param delete_fn  Called once per committed app ID.  Must return true on
  ///                   success, false on failure.
  /// @param on_error   Optional callback invoked for each rollback failure.
  ///                   Typically used to emit an ERROR log.
  /// @return           Number of rollback failures (0 = full rollback success).
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

  /// @return The list of app IDs committed so far (in insertion order).
  const std::vector<std::string>& committed() const { return committed_apps_; }

  /// @return true if at least one app has been marked as committed.
  bool has_committed() const { return !committed_apps_.empty(); }

  /// @return Number of committed app IDs.
  std::size_t committed_count() const { return committed_apps_.size(); }

 private:
  std::vector<std::string> committed_apps_;
};
