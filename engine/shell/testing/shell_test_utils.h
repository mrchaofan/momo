#pragma once

// shell 单测共用的测试替身(无 OS API)。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "base/event_loop.h"
#include "base/object.h"
#include "base/task_runner.h"
#include "engine/platform/render_surface.h"
#include "engine/platform/vsync_source.h"
#include "engine/shell/vsync_waiter.h"

namespace momo::engine::testing {

// 手动驱动的 runner: 认为调用方始终在其线程上, 任务由测试 RunUntilIdle() 执行(忽略延迟)。
// PostTask 可在任意线程调用。
class ManualTaskRunner final : public TaskRunner {
 public:
  void PostTask(Closure task) override {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  void PostDelayedTask(Closure task, TimeDelta) override { PostTask(std::move(task)); }
  bool RunsTasksOnCurrentThread() const override { return true; }

  void RunUntilIdle() {
    while (true) {
      Closure task;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }
  size_t pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<Closure> tasks_;
};

// 由测试手动触发的 vsync。
class FakeVsyncWaiter final : public VsyncWaiter {
 public:
  explicit FakeVsyncWaiter(std::shared_ptr<TaskRunner> ui, int* await_count = nullptr)
      : VsyncWaiter(std::move(ui)), await_count_(await_count) {}

  void Fire() {
    const TimePoint now = Clock::now();
    FireCallback(now, now + std::chrono::milliseconds(16));
  }

 private:
  void AwaitVsync() override {
    if (await_count_ != nullptr) {
      (*await_count_)++;
    }
  }
  int* await_count_;
};

// 计数的软件 surface: 帧真正光栅化进内存缓冲, present 时保存一份快照供测试读取。
// 可在 raster 线程使用;计数器原子, 快照加锁。
class FakeSurface final : public platform::RenderSurface {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size size) override {
    acquired++;
    last_width = size.width;
    if (fail_acquire) {
      return UnavailableError("fake: acquire failed");
    }
    const int w = static_cast<int>(size.width);
    const int h = static_cast<int>(size.height);
    buffer_.assign(static_cast<size_t>(w) * static_cast<size_t>(std::max(h, 0)), 0);
    platform::SurfaceFrame frame;
    frame.kind = platform::SurfaceKind::kSoftware;
    frame.software = {buffer_.data(), static_cast<size_t>(w) * 4, w, h};
    frame.present = [this, w, h] {
      {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_ = buffer_;
        snapshot_width_ = w;
        snapshot_height_ = h;
      }
      presented++;
      return true;
    };
    return frame;
  }

  // 最近一次 present 的像素(预乘 BGRA 打包为 uint32: 0xAARRGGBB, 仅用于不透明像素比对)。
  uint32_t PresentedPixel(int x, int y) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (x < 0 || y < 0 || x >= snapshot_width_ || y >= snapshot_height_) {
      return 0;
    }
    return snapshot_[static_cast<size_t>(y) * snapshot_width_ + x];
  }

  std::atomic<int> acquired{0};
  std::atomic<int> presented{0};
  std::atomic<double> last_width{0};
  std::atomic<bool> fail_acquire{false};

 private:
  std::vector<uint32_t> buffer_;  // 仅 raster 线程
  std::mutex snapshot_mutex_;
  std::vector<uint32_t> snapshot_;
  int snapshot_width_ = 0;
  int snapshot_height_ = 0;
};

// 模拟 CVDisplayLink 的 VsyncSource。momo::Object: 归属创建线程(platform)的 EventLoop。
// 与真实实现相同的线程模型: 独立的"系统 vsync 线程"只负责投递, 回调在归属线程(调用方)上执行
// (vsync_source.h 契约)。析构时 join 全部系统线程;已投递未执行的回调因 WeakPtr 失效而作废。
class FakeVsyncSource final : public platform::VsyncSource {
 public:
  explicit FakeVsyncSource(std::function<void()> on_destroy = nullptr)
      : on_destroy_(std::move(on_destroy)) {}
  ~FakeVsyncSource() override {
    for (std::thread& t : threads_) {
      t.join();
    }
    if (on_destroy_) {
      on_destroy_();
    }
  }
  void RequestFrameCallback(Callback callback) override {
    // 契约: 只在归属线程调用。
    if (!IsOnAffinityThread()) {
      called_off_thread = true;
    }
    requests++;
    threads_.emplace_back([weak = WeakPtr<FakeVsyncSource>(this), callback = std::move(callback)] {
      const TimePoint now = Clock::now();
      PostToObject(weak, [callback, now](FakeVsyncSource& self) {
        if (!self.IsOnAffinityThread()) {
          self.callback_off_thread = true;
        }
        self.callbacks_fired++;
        callback(now, now + std::chrono::milliseconds(16));
      });
    });
  }

  std::atomic<int> requests{0};
  std::atomic<int> callbacks_fired{0};
  std::atomic<bool> called_off_thread{false};
  std::atomic<bool> callback_off_thread{false};

 private:
  std::function<void()> on_destroy_;
  std::vector<std::thread> threads_;  // 仅归属线程访问
};

// 轮询等待条件成立(最多 timeout)。若给出 loop(必须绑定当前线程), 每次轮询前先 RunUntilIdle 一次,
// 让投递到本线程的任务与 DeleteLater 得以执行。
inline bool WaitFor(const std::function<bool()>& condition,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000),
                    EventLoop* loop = nullptr) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (loop != nullptr) {
      loop->RunUntilIdle();
    }
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (loop != nullptr) {
    loop->RunUntilIdle();
  }
  return condition();
}

}  // namespace momo::engine::testing
