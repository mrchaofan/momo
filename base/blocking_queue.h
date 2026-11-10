#pragma once

// BlockingQueue<T>: 线程安全的无界 FIFO, 消费方可阻塞等待。header-only, 属于 momo_base。
//
// 用途: EventLoop 无法服务的场景下的跨线程消息传递 —— 典型是"当前线程被同步阻塞, 但仍要处理
// 来自其他线程的消息"(如 V8 Inspector 断点暂停: ui 线程停在 runMessageLoopOnPause 里, 而
// EventLoop::Run/RunUntilIdle 不可嵌套)。普通跨线程投递请用 TaskRunner, 不要用本类。
//
// 线程约定: 所有方法线程安全, 任意线程调用。生产者与消费者都可以有多个。
// 关闭语义: Close() 之后 Push 被拒绝(返回 false, 值被丢弃);已入队的元素仍可取出,
// 取空后 Pop 立即返回 nullopt —— 用于唤醒并释放阻塞中的消费者。

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "base/macros.h"
#include "base/time.h"

namespace momo {

template <typename T>
class BlockingQueue {
 public:
  BlockingQueue() = default;

  // 入队并唤醒一个等待者。已关闭返回 false。
  bool Push(T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return false;
    }
    items_.push_back(std::move(value));
    cv_.notify_one();
    return true;
  }

  // 阻塞直到有元素(返回之)或队列已关闭且为空(返回 nullopt)。
  std::optional<T> Pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !items_.empty() || closed_; });
    return TakeFrontLocked();
  }

  // 最多等待 timeout;超时、或已关闭且为空时返回 nullopt。
  std::optional<T> PopFor(TimeDelta timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this] { return !items_.empty() || closed_; });
    return TakeFrontLocked();
  }

  // 不等待。
  std::optional<T> TryPop() {
    std::lock_guard<std::mutex> lock(mutex_);
    return TakeFrontLocked();
  }

  // 幂等。唤醒所有等待者。
  void Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.notify_all();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

 private:
  std::optional<T> TakeFrontLocked() {
    if (items_.empty()) {
      return std::nullopt;
    }
    std::optional<T> value(std::move(items_.front()));
    items_.pop_front();
    return value;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<T> items_;
  bool closed_ = false;

  MOMO_DISALLOW_COPY_AND_ASSIGN(BlockingQueue);
};

}  // namespace momo
