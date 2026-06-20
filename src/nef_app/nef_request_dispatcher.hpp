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

#include "logger.hpp"  // Logger::nef_app() — match existing nef_app logging

namespace oai::nef::app {

// Bounded MPSC request dispatcher. Multiple HTTP/2 worker threads enqueue
// (producers); a fixed pool of dispatcher worker threads execute tasks
// (consumers). Each task is a self-contained std::function<void()> that has
// already value-captured the bearer token and the response sink, so no
// thread-local or stack reference crosses the boundary unsafely.
//
// INVARIANT: a dispatcher task must NEVER enqueue onto this same dispatcher
// (no self-enqueue). The dispatcher
// pool should be sized >= the HTTP worker pool to avoid head-of-line blocking
// while HTTP workers are parked on fut.wait(); see constructor warn.
class nef_request_dispatcher {
 public:
  enum class dispatch_status { ok, queue_full, stopped };

  // http_worker_count is passed only to validate the sizing invariant and warn.
  explicit nef_request_dispatcher(
      std::size_t num_threads, std::size_t http_worker_count,
      std::size_t max_queue = 10000)
      : m_max_queue(max_queue), m_running(true) {
    if (num_threads < http_worker_count) {
      Logger::nef_app().warn(
          "nef_request_dispatcher pool ({}) is smaller than the HTTP worker "
          "pool ({}). Under synchronous option this can park HTTP "
          "workers on fut.wait() with no spare dispatcher capacity, hurting "
          "tail latency. Recommended: dispatcher pool >= HTTP pool.",
          num_threads, http_worker_count);
    }
    m_workers.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
      m_workers.emplace_back([this] { worker_loop(); });
  }

  ~nef_request_dispatcher() { stop(); }

  nef_request_dispatcher(const nef_request_dispatcher&) = delete;
  nef_request_dispatcher& operator=(const nef_request_dispatcher&) = delete;

  // Non-blocking. Returns queue_full (caller sends 503) or stopped on shutdown.
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

  // Drain remaining tasks, then join. Idempotent. Must be called only AFTER
  // HTTP intake has stopped (see plan §1.10b) so no HTTP worker is left parked
  // on a future whose task is still queued.
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
      task();  // never holds the lock during user code
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
