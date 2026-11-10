#include "base/worker_pool.h"

#include <algorithm>
#include <utility>

#include "base/thread.h"

namespace momo {

WorkerPool::WorkerPool(std::string label, size_t thread_count) : label_(std::move(label)) {
  if (thread_count == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    thread_count = std::max<size_t>(1, hw > 1 ? hw - 1 : 1);
  }
  threads_.reserve(thread_count);
  for (size_t i = 0; i < thread_count; ++i) {
    threads_.emplace_back([this, i] { WorkerMain(i); });
  }
}

WorkerPool::~WorkerPool() { Shutdown(); }

bool WorkerPool::PostTask(Closure task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shutting_down_) {
      queue_.push_back(std::move(task));
      cv_.notify_one();
      return true;
    }
  }
  // 已关闭: task 在锁外析构。
  return false;
}

bool WorkerPool::PostDelayedTask(Closure task, TimeDelta delay) {
  if (delay <= TimeDelta::zero()) {
    return PostTask(std::move(task));
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shutting_down_) {
      delayed_.emplace(Clock::now() + delay, std::move(task));
      cv_.notify_one();  // 可能比当前等待的最早时刻更早
      return true;
    }
  }
  return false;
}

void WorkerPool::Shutdown() {
  std::multimap<TimePoint, Closure> dropped;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_ && threads_.empty()) {
      return;
    }
    shutting_down_ = true;
    dropped.swap(delayed_);  // 未到期的延迟任务丢弃, 在锁外析构
  }
  cv_.notify_all();
  for (std::thread& t : threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  threads_.clear();
}

void WorkerPool::WorkerMain(size_t index) {
  Thread::SetCurrentThreadName(label_ + ".worker." + std::to_string(index));
  while (true) {
    Closure task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      while (true) {
        // 到期的延迟任务转入普通队列。
        const TimePoint now = Clock::now();
        while (!delayed_.empty() && delayed_.begin()->first <= now) {
          queue_.push_back(std::move(delayed_.begin()->second));
          delayed_.erase(delayed_.begin());
        }
        if (!queue_.empty()) {
          break;
        }
        if (shutting_down_) {
          return;  // 已排空(Shutdown 已清掉未到期任务)
        }
        if (delayed_.empty()) {
          cv_.wait(lock);
        } else {
          cv_.wait_until(lock, delayed_.begin()->first);
        }
      }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    task();
  }
}

}  // namespace momo
