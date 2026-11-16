#pragma once

// QueueNativeLoop: 不依赖任何系统消息循环的 NativeLoop——调用 Run 的线程上的一个定时任务队列。
// 给没有原生窗口系统的宿主用(headless 运行器、测试的 mock 平台): 它们同样经 NativeMessagePump 在其上运行
// platform EventLoop, 与真实平台走同一条路。
// 线程约定: 见 base/native_loop.h(PostTask / PostDelayedTask / Quit 任意线程;Run 在创建它的线程)。

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "base/closure.h"
#include "base/native_loop.h"
#include "base/time.h"

namespace momo {

class QueueNativeLoop : public NativeLoop {
 public:
  QueueNativeLoop() : main_id_(std::this_thread::get_id()) {}

  void PostTask(Closure task) override { PostDelayedTask(std::move(task), TimeDelta::zero()); }
  void PostDelayedTask(Closure task, TimeDelta delay) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.emplace(std::make_pair(Clock::now() + delay, next_seq_++), std::move(task));
    }
    cv_.notify_one();
  }
  bool RunsTasksOnCurrentThread() const override {
    return std::this_thread::get_id() == main_id_;
  }
  // 支持嵌套(模态循环): 每次 Quit 只让最内层的 Run 返回。未运行时调用 Quit: 下一次 Run 立即返回。
  void Run() override {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      if (quit_) {
        quit_ = false;
        break;
      }
      if (queue_.empty()) {
        cv_.wait(lock);
        continue;
      }
      auto it = queue_.begin();
      if (it->first.first > Clock::now()) {
        cv_.wait_until(lock, it->first.first);
        continue;
      }
      Closure task = std::move(it->second);
      queue_.erase(it);
      lock.unlock();
      task();
      task = nullptr;
      lock.lock();
    }
  }
  void Quit() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
    }
    cv_.notify_one();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<std::pair<TimePoint, uint64_t>, Closure> queue_;  // (到期时间, 投递序号) → 任务
  uint64_t next_seq_ = 0;
  bool quit_ = false;
  const std::thread::id main_id_;
};

}  // namespace momo
