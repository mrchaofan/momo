#include "engine/shell/vsync_waiter.h"

#include <utility>

#include "base/logging.h"

namespace momo::engine {

// ---------------------------------------------------------------------------
// VsyncWaiter

VsyncWaiter::VsyncWaiter(std::shared_ptr<TaskRunner> ui_runner) : Object(std::move(ui_runner)) {}

void VsyncWaiter::AsyncWaitForVsync(Callback callback) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_DCHECK(static_cast<bool>(callback));
  const bool already_waiting = is_waiting();
  callback_ = std::move(callback);
  if (!already_waiting) {
    AwaitVsync();
  }
}

void VsyncWaiter::FireCallback(TimePoint frame_start, TimePoint frame_target) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (!callback_) {
    return;
  }
  Callback callback = std::move(callback_);
  callback_ = nullptr;
  // callback 中可以再次 AsyncWaitForVsync(甚至导致本对象被替换销毁), 之后不再访问成员。
  callback(frame_start, frame_target);
}

// ---------------------------------------------------------------------------
// TimerVsyncWaiter

TimerVsyncWaiter::TimerVsyncWaiter(std::shared_ptr<TaskRunner> ui_runner, double refresh_rate)
    : VsyncWaiter(std::move(ui_runner)), phase_(Clock::now()) {
  const double hz = refresh_rate > 0 ? refresh_rate : kDefaultRefreshRate;
  interval_ = std::chrono::duration_cast<TimeDelta>(std::chrono::duration<double>(1.0 / hz));
}

void TimerVsyncWaiter::AwaitVsync() {
  const TimePoint now = Clock::now();
  // 下一个节拍: phase_ + k * interval_ > now。
  const auto ticks = (now - phase_) / interval_ + 1;
  const TimePoint next = phase_ + ticks * interval_;
  ui_runner()->PostDelayedTask(
      [weak = WeakPtr<TimerVsyncWaiter>(this), next, interval = interval_] {
        if (TimerVsyncWaiter* self = weak.get()) {
          self->FireCallback(next, next + interval);
        }
      },
      next - now);
}

// ---------------------------------------------------------------------------
// PlatformVsyncWaiter

PlatformVsyncWaiter::PlatformVsyncWaiter(std::shared_ptr<TaskRunner> ui_runner,
                                         WeakPtr<platform::VsyncSource> source)
    : VsyncWaiter(std::move(ui_runner)), source_(std::move(source)) {
  MOMO_CHECK(source_.is_bound()) << "PlatformVsyncWaiter requires a source";
}

void PlatformVsyncWaiter::AwaitVsync() {
  // VsyncSource 只能在其归属线程(platform)调用: 投递过去, 判活后请求。
  // 平台回调按契约在 platform 线程执行;本 waiter 的回调必须回到发起请求的 ui 线程:
  // 只携带 WeakPtr, PostToObject 回 ui 判活后再 FireCallback。
  platform::VsyncSource::Callback on_vsync =
      [weak = WeakPtr<PlatformVsyncWaiter>(this)](TimePoint start, TimePoint target) {
        PostToObject(weak, [start, target](PlatformVsyncWaiter& self) {
          self.FireCallback(start, target);
        });
      };
  PostToObject(source_, [on_vsync = std::move(on_vsync)](platform::VsyncSource& source) mutable {
    source.RequestFrameCallback(std::move(on_vsync));
  });
}

}  // namespace momo::engine
