/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "logger.hpp"

namespace oai::nef::app {

// Bounded request dispatcher: one task queue served by a pool of worker
// threads.
//
// Producers are the HTTP/2 worker threads; consumers are a fixed pool of
// dispatcher worker threads. Each task is a self-contained
// std::function<void()> that has already captured the bearer token and the
// response sink by value, so no thread-local or stack reference crosses
// threads unsafely.
//
// Two rules apply:
//
//  - No self-enqueue. A dispatcher task must never enqueue work onto this
//    same dispatcher: with every worker blocked on its own nested task, the
//    pool deadlocks.
//
//  - Make the dispatcher pool at least as large as the HTTP worker pool. On
//    the synchronous path an HTTP worker waits on fut.wait() while its task
//    runs, so a smaller dispatcher pool leaves no spare capacity and requests
//    queue up behind each other (head-of-line blocking). The constructor logs
//    a warning when the configured sizes break this rule.
class nef_request_dispatcher {
 public:
  enum class dispatch_status { ok, queue_full, stopped };

  // http_worker_count is used only to check the sizing rule above and warn;
  // it does not affect how many dispatcher threads are started.
  // max_queue caps the backlog: once it is reached, dispatch() rejects new
  // tasks instead of growing the queue.
  explicit nef_request_dispatcher(
      std::size_t num_threads, std::size_t http_worker_count,
      std::size_t max_queue = 10000)
      : m_max_queue(max_queue), m_running(true) {
    if (num_threads < http_worker_count) {
      Logger::nef_app().warn(
          "nef_request_dispatcher pool (%zu) is smaller than the HTTP worker "
          "pool (%zu). Under synchronous option this can park HTTP "
          "workers on fut.wait() with no spare dispatcher capacity, hurting "
          "tail latency. Recommended: dispatcher pool >= HTTP pool.",
          num_threads, http_worker_count);
    }
    m_workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
      m_workers.emplace_back([this] { worker_loop(); });
  }

  ~nef_request_dispatcher() { stop(); }

  nef_request_dispatcher(const nef_request_dispatcher&)            = delete;
  nef_request_dispatcher& operator=(const nef_request_dispatcher&) = delete;

  // Non-blocking: enqueues and returns, never waits for a worker.
  // Returns queue_full once the backlog hits max_queue (the caller answers
  // 503), or stopped once stop() has run.
  dispatch_status dispatch(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lk(m_mutex);
      if (!m_running) return dispatch_status::stopped;
      if (m_queue.size() >= m_max_queue) return dispatch_status::queue_full;
      m_queue.push(std::move(task));
    }
    m_cv.notify_one();
    return dispatch_status::ok;
  }

  // Stop accepting tasks, run every task already queued, then join the
  // workers. Idempotent.
  //
  // Call it only after HTTP intake has stopped. A request that arrives after
  // this point is refused: dispatch() returns stopped and the caller answers
  // 503.
  void stop() {
    {
      std::lock_guard<std::mutex> lk(m_mutex);
      if (!m_running) return;
      m_running = false;
    }
    m_cv.notify_all();
    for (auto& t : m_workers)
      if (t.joinable()) t.join();
    m_workers.clear();
  }

  // Tasks still waiting. Tasks already picked up by a worker are not counted,
  // so 0 does not mean idle.
  std::size_t queue_depth() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_queue.size();
  }

 private:
  void worker_loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv.wait(lk, [this] { return !m_running || !m_queue.empty(); });
        if (!m_running && m_queue.empty()) return;
        task = std::move(m_queue.front());
        m_queue.pop();
      }
      // Last-resort safety net only. Request errors should be handled by the
      // per-handler try/catch, which calls the response_sink so the caller
      // gets a proper HTTP status. If an exception reaches here, the sink was
      // never called and a blocking caller may hang. That is a bug, hence the
      // "BUG:" error log. The exception is still swallowed to keep the worker
      // (and the process) alive: an exception escaping task() would call
      // std::terminate and abort the whole NF.
      try {
        task();  // outside the lock: user code must not block the other workers
      } catch (const std::exception& e) {
        Logger::nef_app().error(
            "BUG: dispatcher task threw an exception that escaped every "
            "handler; the response sink was NOT invoked so the caller may "
            "hang. Fix the offending handler. what(): %s",
            e.what());
      } catch (...) {
        Logger::nef_app().error(
            "BUG: dispatcher task threw a non-std exception that escaped every "
            "handler; the response sink was NOT invoked so the caller may "
            "hang. Fix the offending handler.");
      }
    }
  }

  std::size_t m_max_queue;
  bool m_running;
  std::vector<std::thread> m_workers;
  std::queue<std::function<void()>> m_queue;
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
};

}  // namespace oai::nef::app
