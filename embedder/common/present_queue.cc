#include "embedder/common/present_queue.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include "base/event_loop.h"
#include "base/logging.h"

namespace momo::embedder {

std::shared_ptr<PresentQueue> PresentQueue::Create(Wake wake) {
  return std::shared_ptr<PresentQueue>(new PresentQueue(std::move(wake)));
}

PresentQueue::PresentQueue(Wake wake) : wake_(std::move(wake)) {}

uint64_t PresentQueue::PackSize(Size size) {
  const auto width = static_cast<uint32_t>(std::max(0, static_cast<int>(size.width)));
  const auto height = static_cast<uint32_t>(std::max(0, static_cast<int>(size.height)));
  return (static_cast<uint64_t>(width) << 32) | height;
}

void PresentQueue::Post(Size size, std::function<void()> present) {
  pending_.fetch_add(1, std::memory_order_acq_rel);
  queue_.Push({PackSize(size), std::move(present)});
  // 先入队再判断: Drain 先清标记再取帧, 所以这一帧要么被在途的 Drain 取走, 要么触发新的唤醒。
  if (!wake_pending_.exchange(true, std::memory_order_acq_rel) && wake_) {
    wake_(shared_from_this());
  }
}

void PresentQueue::NotePresented(Size size) {
  presented_size_.store(PackSize(size), std::memory_order_release);
}

void PresentQueue::Run(Entry& entry) {
  if (entry.present) {
    entry.present();
  }
  presented_size_.store(entry.size, std::memory_order_release);
  pending_.fetch_sub(1, std::memory_order_acq_rel);
}

bool PresentQueue::RunQueued(uint64_t target) {
  wake_pending_.store(false, std::memory_order_release);
  bool matched = false;
  while (std::optional<Entry> entry = queue_.TryPop()) {
    Run(*entry);
    matched = matched || entry->size == target;
  }
  return matched;
}

void PresentQueue::Drain() { RunQueued(0); }

bool PresentQueue::WaitForFrame(Size size, TimeDelta timeout, const std::function<bool()>& pump) {
  // pump 里窗口可能被销毁(连同它持有的引用): 等待期间自己持有一份。
  const std::shared_ptr<PresentQueue> keep = shared_from_this();
  const uint64_t target = PackSize(size);
  const auto arrived = [&] {
    return RunQueued(target) || presented_size_.load(std::memory_order_acquire) == target;
  };
  const TimePoint deadline = Clock::now() + timeout;
  while (true) {
    if (arrived()) {
      return true;
    }
    const TimePoint now = Clock::now();
    if (now >= deadline) {
      return false;
    }
    if (!pump()) {
      return false;
    }
    if (arrived()) {
      return true;
    }
    // 等下一帧入队;在 raster 线程呈现的帧不入队, 所以只等很短的一段就回头再查。
    const TimeDelta slice = std::min<TimeDelta>(std::chrono::milliseconds(2), deadline - now);
    if (std::optional<Entry> entry = queue_.PopFor(slice)) {
      Run(*entry);
      if (entry->size == target) {
        RunQueued(0);  // 其后已入队的帧按序一并呈现
        return true;
      }
    }
  }
}

void PresentQueue::SynchronizeResize(Size size, const std::function<void()>& notify_metrics) {
  if (resize_waiting_ || !has_presented()) {
    notify_metrics();
    return;
  }
  const std::shared_ptr<PresentQueue> keep = shared_from_this();
  resize_waiting_ = true;
  // 先把新尺寸交给引擎, 再等它画出这个尺寸的帧: 系统回调返回后才显示新尺寸的窗口, 窗口与内容同时变化。
  notify_metrics();
  // 引擎出下一帧要经过 platform 线程的任务(vsync 请求与回调、平台视图呈现): 不执行它们会互等。
  EventLoop* loop = EventLoop::Current();
  const bool got_frame = WaitForFrame(size, kLiveResizeFrameWait, [loop] {
    if (loop != nullptr) {
      loop->RunDueTasksNested();
    }
    return true;
  });
  if (!got_frame) {
    MOMO_LOG(VERBOSE) << "Live resize: no frame of the new size within the timeout; content is stretched.";
  }
  resize_waiting_ = false;
}

}  // namespace momo::embedder
