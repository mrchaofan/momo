#pragma once

// 测试用 NativeLoop。base 与上层测试(embedder 的 mock 平台)共用。

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#include "base/closure.h"
#include "base/native_loop.h"
#include "base/time.h"

namespace momo::testing {

// 模拟 OS 原生消息循环(NSApp/Win32 泵): 跑在测试线程上的简单定时队列, 不使用 momo::EventLoop
// (EventLoop 经 NativeMessagePump 建在它之上)。
class MockNativeLoop final : public NativeLoop {
 public:
  MockNativeLoop() : main_id_(std::this_thread::get_id()) {}

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
  // 支持嵌套(模拟模态框): 每次 Quit 只让最内层的 RunMessageLoop 返回。
  void Run() override {
    run_count_++;
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
    quit_count_++;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
    }
    cv_.notify_one();
  }

  int run_count() const { return run_count_; }
  int quit_count() const { return quit_count_; }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<std::pair<TimePoint, uint64_t>, Closure> queue_;
  uint64_t next_seq_ = 0;
  bool quit_ = false;
  std::thread::id main_id_;
  int run_count_ = 0;
  int quit_count_ = 0;
};

}  // namespace momo::testing
