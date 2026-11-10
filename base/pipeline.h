#pragma once

// Pipeline<T>: 固定深度的帧生产/消费管道, 实现帧管线背压(docs/05 §5, docs/08 §4)。
// header-only, 属于 momo_base。
//
// 深度 = "排队 + 正在消费" 的上限。Shell 使用深度 2(队列 1 + 正在绘制 1)。
//  - 生产者(ui/Animator)先 Produce() 预占一个槽位;拿不到(返回空 continuation)说明消费者落后,
//    应跳过本次 vsync(丢 build 不丢最新画面)。
//  - continuation.Complete(item) 把产物入队;continuation 未 Complete 即析构则归还槽位。
//  - 消费者(raster)Consume(fn): 取出最早的产物调用 fn, fn 返回后才归还槽位。
//
// 线程约定: 所有方法线程安全(内部小临界区, fn 在锁外执行)。以 shared_ptr 共享,
// continuation 持有 Pipeline 的共享引用, 因此可以安全地比创建者活得久。

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "base/logging.h"
#include "base/macros.h"

namespace momo {

enum class PipelineConsumeResult {
  kNoItem,         // 队列为空, fn 未被调用
  kDone,           // 已消费一个, 队列现在为空
  kMoreAvailable,  // 已消费一个, 队列中还有
};

template <typename T>
class Pipeline : public std::enable_shared_from_this<Pipeline<T>> {
 public:
  class ProducerContinuation {
   public:
    ProducerContinuation() = default;
    ProducerContinuation(ProducerContinuation&& other) noexcept
        : pipeline_(std::move(other.pipeline_)) {}
    ProducerContinuation& operator=(ProducerContinuation&& other) noexcept {
      if (this != &other) {
        Release();
        pipeline_ = std::move(other.pipeline_);
      }
      return *this;
    }
    ~ProducerContinuation() { Release(); }

    explicit operator bool() const { return pipeline_ != nullptr; }

    // 入队产物。返回 false 表示 continuation 为空(未预占到槽位)。之后 continuation 变为空。
    bool Complete(T item) {
      if (!pipeline_) {
        return false;
      }
      std::shared_ptr<Pipeline> pipeline = std::move(pipeline_);
      pipeline->Enqueue(std::move(item));
      return true;
    }

   private:
    friend class Pipeline;
    explicit ProducerContinuation(std::shared_ptr<Pipeline> pipeline)
        : pipeline_(std::move(pipeline)) {}

    void Release() {
      if (pipeline_) {
        std::shared_ptr<Pipeline> pipeline = std::move(pipeline_);
        pipeline->ReleaseSlot();
      }
    }

    std::shared_ptr<Pipeline> pipeline_;

    MOMO_DISALLOW_COPY_AND_ASSIGN(ProducerContinuation);
  };

  static std::shared_ptr<Pipeline> Create(size_t depth) {
    MOMO_CHECK(depth > 0) << "Pipeline depth must be positive";
    return std::shared_ptr<Pipeline>(new Pipeline(depth));  // NOLINT: 私有构造
  }

  // 预占一个槽位。管道已满时返回空 continuation。
  ProducerContinuation Produce() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (available_slots_ == 0) {
        return ProducerContinuation();
      }
      --available_slots_;
    }
    return ProducerContinuation(this->shared_from_this());
  }

  // 取出最早的产物交给 fn(在调用线程、锁外执行), fn 返回后归还槽位。
  PipelineConsumeResult Consume(const std::function<void(T)>& fn) {
    T item;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty()) {
        return PipelineConsumeResult::kNoItem;
      }
      item = std::move(queue_.front());
      queue_.pop_front();
    }
    fn(std::move(item));
    std::lock_guard<std::mutex> lock(mutex_);
    ++available_slots_;
    return queue_.empty() ? PipelineConsumeResult::kDone : PipelineConsumeResult::kMoreAvailable;
  }

  size_t depth() const { return depth_; }
  size_t available_slots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_slots_;
  }
  size_t queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

 private:
  explicit Pipeline(size_t depth) : depth_(depth), available_slots_(depth) {}

  void Enqueue(T item) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(item));
  }

  void ReleaseSlot() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++available_slots_;
  }

  const size_t depth_;
  mutable std::mutex mutex_;
  size_t available_slots_;
  std::deque<T> queue_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Pipeline);
};

}  // namespace momo
