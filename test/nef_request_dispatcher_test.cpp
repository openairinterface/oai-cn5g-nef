/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
//
// Unit tests for nef_request_dispatcher (NEF async-comm refactor, Phase 1).
//
// Covers (plan §1.9):
//   1. dispatch N tasks -> all run (atomic counter == N after stop()).
//   2. dispatch returns queue_full when backlog exceeds max_queue.
//   3. stop() drains pending tasks before returning.
//   4. Bearer-token isolation: each task sets and reads back its own
//      thread_local value; a worker reused for a second task sees the cleared
//      (empty) value before re-set. This proves the thread_local re-set/clear
//      discipline that nef_app::{set,clear,get}_request_bearer_token() relies on
//      (the accessors are a thin wrapper over a file-local thread_local string,
//      so the property is exercised here with the identical set/read/clear
//      pattern without paying nef_app's heavy construction cost — NRF
//      registration, timers, global nef_config_inst — per the §1.9 fallback).
//   5. Undersize warning: num_threads < http_worker_count -> still runs, no throw.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "nef_request_dispatcher.hpp"

using oai::nef::app::nef_request_dispatcher;

// The dispatcher's undersize-warning path calls Logger::nef_app(), which throws
// "Logger nef_app does not exist" unless Logger::init() ran first. Initialise it
// once via the C-linkage hook in test_logger_init.cpp (same pattern Group B
// uses) through a GoogleTest global environment.
extern "C" void nef_test_init_logger();
namespace {
class LoggerEnv : public ::testing::Environment {
  void SetUp() override { nef_test_init_logger(); }
};
const ::testing::Environment* const kLoggerEnv =
    ::testing::AddGlobalTestEnvironment(new LoggerEnv());
}  // namespace

// 1. All dispatched tasks eventually run; stop() guarantees they finished.
TEST(NefRequestDispatcher, RunsAllTasks) {
  nef_request_dispatcher d(2, 2, 1000);
  std::atomic<int> count{0};
  for (int i = 0; i < 10; ++i)
    d.dispatch([&count] { count.fetch_add(1, std::memory_order_relaxed); });
  d.stop();
  EXPECT_EQ(count.load(), 10);
}

// 2. dispatch() returns queue_full once the backlog reaches max_queue.
TEST(NefRequestDispatcher, QueueFullReturnsStatus) {
  // 1 worker, queue capacity 3. The first task the single worker picks up
  // blocks on the gate; the rest pile up in the queue.
  nef_request_dispatcher d(1, 1, 3);
  std::atomic<bool> gate{false};
  std::atomic<int> started{0};

  auto blocking_task = [&gate, &started] {
    started.fetch_add(1, std::memory_order_relaxed);
    while (!gate.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  };

  // Wait until the worker has actually dequeued and is blocked on the first
  // task, so the remaining capacity is deterministic.
  ASSERT_EQ(d.dispatch(blocking_task),
            nef_request_dispatcher::dispatch_status::ok);
  while (started.load(std::memory_order_relaxed) < 1)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

  // The worker is now blocked; fill the queue to capacity (3 entries).
  for (int i = 0; i < 3; ++i)
    ASSERT_EQ(d.dispatch(blocking_task),
              nef_request_dispatcher::dispatch_status::ok);

  // Queue is full (size == max_queue == 3); the next dispatch is rejected.
  EXPECT_EQ(d.dispatch([] {}),
            nef_request_dispatcher::dispatch_status::queue_full);

  // Release the workers so stop() can drain/join cleanly.
  gate.store(true, std::memory_order_release);
  d.stop();
}

// 3. stop() drains all queued tasks before returning.
TEST(NefRequestDispatcher, StopDrainsPending) {
  nef_request_dispatcher d(1, 1, 100);
  std::atomic<int> count{0};
  for (int i = 0; i < 5; ++i)
    d.dispatch([&count] {
      // Small delay so several tasks are still queued when stop() is called.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      count.fetch_add(1, std::memory_order_relaxed);
    });
  d.stop();  // must drain all 5 before returning
  EXPECT_EQ(count.load(), 5);
}

// 4. Bearer-token isolation through the dispatcher.
//
// Mirrors nef_app's accessor semantics exactly: a thread_local string that each
// task sets, reads back, and clears. We assert (a) each task reads its own
// value, never another task's, and (b) a worker reused for a second task sees
// the empty (cleared) value before it re-sets — the discipline §1.6 mandates.
namespace {
// Same shape as nef_app.cpp's `thread_local std::string g_request_bearer_token`.
thread_local std::string tls_bearer_token;
void set_token(const std::string& t) { tls_bearer_token = t; }
std::string get_token() { return tls_bearer_token; }
void clear_token() { tls_bearer_token.clear(); }
}  // namespace

TEST(NefRequestDispatcher, BearerTokenIsolation) {
  // 2 workers so the two "concurrent" tokens can land on distinct threads.
  nef_request_dispatcher d(2, 2, 1000);

  std::atomic<bool> a_read_ok{false};
  std::atomic<bool> b_read_ok{false};
  std::atomic<bool> reuse_saw_empty{true};  // false if any reuse saw a stale token

  auto make_task = [&](const std::string& token, std::atomic<bool>& read_ok) {
    return [&, token] {
      // On a reused worker the previous task must have cleared the token.
      if (!get_token().empty()) reuse_saw_empty.store(false);
      set_token(token);
      // Hold long enough that the sibling task is likely running concurrently.
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      // Each task must read back exactly its own value, never the sibling's.
      if (get_token() == token) read_ok.store(true);
      clear_token();  // discipline: clear before the worker is reused
    };
  };

  d.dispatch(make_task("token-A", a_read_ok));
  d.dispatch(make_task("token-B", b_read_ok));

  // A second round to exercise worker reuse: each reused worker must observe an
  // empty token at entry (set by clear_token() in the prior task on that thread).
  std::atomic<bool> dummy{false};
  for (int i = 0; i < 8; ++i)
    d.dispatch(make_task("token-reuse-" + std::to_string(i), dummy));

  d.stop();

  EXPECT_TRUE(a_read_ok.load()) << "task A did not read back its own token";
  EXPECT_TRUE(b_read_ok.load()) << "task B did not read back its own token";
  EXPECT_TRUE(reuse_saw_empty.load())
      << "a reused worker saw a stale (uncleared) bearer token";
}

// 5. Undersize pool (num_threads < http_worker_count) warns but does not throw,
//    and still runs tasks.
TEST(NefRequestDispatcher, UndersizeNoThrow) {
  std::atomic<int> count{0};
  EXPECT_NO_THROW({
    nef_request_dispatcher d(1, 4, 100);  // 1 < 4 — warns, must not throw
    d.dispatch([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    d.stop();
  });
  EXPECT_EQ(count.load(), 1);
}

// 6. dispatch() after stop() reports stopped (queue lifecycle sanity).
TEST(NefRequestDispatcher, DispatchAfterStopReportsStopped) {
  nef_request_dispatcher d(1, 1, 100);
  d.stop();
  EXPECT_EQ(d.dispatch([] {}),
            nef_request_dispatcher::dispatch_status::stopped);
}
