/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace oai::nef::app {

/**
 * Bounded producer-consumer thread pool for notification forwarding.
 *
 * - Fixed number of worker threads (default 4).
 * - Bounded task queue (default 1000 entries). enqueue() returns false once
 *   it is full, so the caller gets backpressure it can log or drop on.
 * - enqueue() never blocks the calling thread.
 * - stop() drains the remaining tasks before joining the workers, so
 *   in-flight notifications still complete on shutdown.
 *
 * TODO: expose num_threads / max_queue as nef_config parameters.
 */
class notification_thread_pool {
 public:
  explicit notification_thread_pool(
      size_t num_threads = 4, size_t max_queue = 1000)
      : m_max_queue(max_queue), m_running(true) {
    for (size_t i = 0; i < num_threads; ++i)
      m_workers.emplace_back([this] { worker_loop(); });
  }

  ~notification_thread_pool() { stop(); }

  // Enqueue a task. Returns false when the queue is full or the pool has
  // stopped; the caller should log and drop.
  bool enqueue(std::function<void()> task) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_running || m_queue.size() >= m_max_queue) return false;
    m_queue.push(std::move(task));
    m_cv.notify_one();
    return true;
  }

  // Drain the remaining tasks, then join all workers. Idempotent.
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

  // Tasks still waiting. Tasks already picked up by a worker are not counted.
  size_t queue_depth() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_queue.size();
  }

 private:
  void worker_loop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv.wait(lk, [this] { return !m_running || !m_queue.empty(); });
        if (!m_running && m_queue.empty()) return;
        task = std::move(m_queue.front());
        m_queue.pop();
      }
      task();
    }
  }

  size_t m_max_queue;
  bool m_running;
  std::vector<std::thread> m_workers;
  std::queue<std::function<void()>> m_queue;
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
};

}  // namespace oai::nef::app
