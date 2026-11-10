#pragma once

// WaitableEvent: 手动复位事件。仅用于启动/关闭等有限几次线程汇合(docs/08 §3),
// 帧路径禁止使用。header-only。

#include <condition_variable>
#include <mutex>

#include "base/macros.h"

namespace momo {

class WaitableEvent {
 public:
  WaitableEvent() = default;

  void Signal() {
    // 必须持锁 notify: 等待方常把事件放在栈上, 醒来即返回并销毁事件;
    // 若先解锁再 notify, notify 可能落在已销毁的 cv 上。
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = true;
    cv_.notify_all();
  }

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return signaled_; });
  }

  bool IsSignaled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return signaled_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool signaled_ = false;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WaitableEvent);
};

}  // namespace momo
