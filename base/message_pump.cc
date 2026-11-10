#include "base/message_pump.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

#include "base/logging.h"
#include "base/macros.h"

namespace momo {

namespace {

class DefaultMessagePump final : public MessagePump {
 public:
  DefaultMessagePump() = default;

  void Attach(Delegate* delegate) override { delegate_ = delegate; }

  void Run() override {
    MOMO_CHECK(delegate_ != nullptr);
    std::unique_lock<std::mutex> lock(mutex_);
    quit_ = false;
    while (true) {
      // 先清掉"已请求"状态再 DoWork: DoWork 期间到达的请求会保留到下面的等待。
      work_ = false;
      delayed_ = TimePoint::max();
      lock.unlock();
      const TimePoint next = delegate_->DoWork();
      lock.lock();
      if (quit_) {
        break;
      }
      const TimePoint wake = std::min(next, delayed_);
      if (work_) {
        continue;
      }
      // 有更早的 ScheduleDelayedWork 到达时也醒来, 重新计算等待时刻(下一轮会再 DoWork 一次)。
      auto woken = [this, wake] { return work_ || quit_ || delayed_ < wake; };
      if (wake == TimePoint::max()) {
        cv_.wait(lock, woken);
      } else {
        cv_.wait_until(lock, wake, woken);
      }
    }
  }

  void Quit() override {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }

  void ScheduleWork() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      work_ = true;
    }
    cv_.notify_one();
  }

  void ScheduleDelayedWork(TimePoint when) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (when >= delayed_) {
        return;
      }
      delayed_ = when;
      if (when <= Clock::now()) {
        work_ = true;
      }
    }
    cv_.notify_one();
  }

 private:
  Delegate* delegate_ = nullptr;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool work_ = false;
  bool quit_ = false;
  TimePoint delayed_ = TimePoint::max();

  MOMO_DISALLOW_COPY_AND_ASSIGN(DefaultMessagePump);
};

}  // namespace

std::unique_ptr<MessagePump> CreateDefaultMessagePump() {
  return std::make_unique<DefaultMessagePump>();
}

}  // namespace momo
