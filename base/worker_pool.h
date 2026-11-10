#pragma once

// WorkerPool: CPU 密集一次性任务的固定大小线程池(docs/08 §1)。不保证执行顺序, 不可调用 JS。
// 线程约定: PostTask/PostDelayedTask 线程安全;构造/Shutdown/析构在宿主线程。析构隐式 Shutdown。
// 关闭语义: Shutdown 之前已到期的任务全部执行完毕后 join;尚未到期的延迟任务与之后的投递被丢弃
// (之后的投递在投递线程析构, 未到期的延迟任务在 Shutdown 调用线程析构)。

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/closure.h"
#include "base/macros.h"
#include "base/time.h"

namespace momo {

class WorkerPool {
 public:
  // thread_count 为 0 时取 max(1, hardware_concurrency - 1)。线程名 "<label>.worker.<i>"。
  explicit WorkerPool(std::string label = "momo", size_t thread_count = 0);
  ~WorkerPool();

  // 返回 false 表示已关闭、任务被丢弃。
  bool PostTask(Closure task);
  // 不早于 delay 之后执行(由空闲 worker 计时, 不额外占线程)。
  bool PostDelayedTask(Closure task, TimeDelta delay);

  // 排空队列后 join 全部线程。幂等。
  void Shutdown();

  size_t thread_count() const { return threads_.size(); }

 private:
  void WorkerMain(size_t index);

  std::string label_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Closure> queue_;
  std::multimap<TimePoint, Closure> delayed_;  // 到期后移入 queue_
  bool shutting_down_ = false;
  std::vector<std::thread> threads_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WorkerPool);
};

}  // namespace momo
