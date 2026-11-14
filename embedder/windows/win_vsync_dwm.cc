#include "embedder/windows/win_vsync_dwm.h"

#include "embedder/windows/win_util.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

#include "base/logging.h"

namespace momo::embedder::windows {

namespace {

constexpr TimeDelta kDefaultPeriod =
    std::chrono::duration_cast<TimeDelta>(std::chrono::microseconds(16667));

TimeDelta QpcToDelta(int64_t ticks, int64_t frequency) {
  if (frequency <= 0) {
    return TimeDelta::zero();
  }
  // 拆成整秒与余数, 避免 ticks * 1e9 溢出。
  const int64_t seconds = ticks / frequency;
  const int64_t remainder = ticks % frequency;
  const auto ns = std::chrono::seconds(seconds) +
                  std::chrono::nanoseconds(remainder * 1000000000LL / frequency);
  return std::chrono::duration_cast<TimeDelta>(ns);
}

void SleepFor(TimeDelta delta) {
  const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
  if (ms > 0) {
    ::Sleep(static_cast<DWORD>(std::min<int64_t>(ms, 1000)));
  }
}

}  // namespace

StatusOr<ObjectPtr<platform::VsyncSource>> WinVsyncDwm::Create() {
  ObjectPtr<WinVsyncDwm> source = MakeObject<WinVsyncDwm>();
  return ObjectPtr<platform::VsyncSource>(source.release());
}

WinVsyncDwm::WinVsyncDwm()
    : shared_(std::make_unique<Shared>()), thread_(std::make_unique<Thread>("momo.vsync")) {
  shared_->owner = WeakPtr<WinVsyncDwm>(this);
  thread_->GetTaskRunner()->PostTask(
      [] { ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); });
}

WinVsyncDwm::~WinVsyncDwm() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  {
    MutexLock lock(shared_->lock);
    shared_->destroyed = true;  // 之后的 Tick 什么也不做, 不再投递
  }
  // 排队中的 Tick 看到 destroyed 立即返回;在途的 DwmFlush 最多阻塞一个刷新周期。
  thread_->Join();
  thread_.reset();
}

void WinVsyncDwm::RequestFrameCallback(Callback callback) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  callback_ = std::move(callback);  // 一次性: 新请求覆盖未触发的旧请求
  bool start = false;
  {
    MutexLock lock(shared_->lock);
    shared_->wanted = true;
    if (!shared_->running) {
      shared_->running = true;
      start = true;
    }
  }
  if (start) {
    Shared* shared = shared_.get();
    TaskRunner* runner = thread_->GetTaskRunner().get();
    runner->PostTask([shared, runner] { Tick(shared, runner); });
  }
}

void WinVsyncDwm::Tick(Shared* shared, TaskRunner* runner) {
  {
    MutexLock lock(shared->lock);
    if (shared->destroyed) {
      shared->running = false;
      return;
    }
  }

  // 阻塞到下一次 DWM 合成(≈ vblank)。
  const HRESULT flush = ::DwmFlush();

  LARGE_INTEGER frequency{};
  LARGE_INTEGER qpc_now{};
  ::QueryPerformanceFrequency(&frequency);
  ::QueryPerformanceCounter(&qpc_now);
  TimePoint now = Clock::now();

  TimeDelta period = kDefaultPeriod;
  TimePoint frame_start = now;
  DWM_TIMING_INFO timing{};
  timing.cbSize = sizeof(timing);
  // Win8.1+ 要求 hwnd 为 NULL(全局合成时序)。
  if (SUCCEEDED(::DwmGetCompositionTimingInfo(nullptr, &timing))) {
    if (timing.qpcRefreshPeriod > 0) {
      period = QpcToDelta(static_cast<int64_t>(timing.qpcRefreshPeriod), frequency.QuadPart);
    } else if (timing.rateRefresh.uiNumerator > 0 && timing.rateRefresh.uiDenominator > 0) {
      period = std::chrono::duration_cast<TimeDelta>(std::chrono::duration<double>(
          static_cast<double>(timing.rateRefresh.uiDenominator) /
          static_cast<double>(timing.rateRefresh.uiNumerator)));
    }
    if (period <= TimeDelta::zero()) {
      period = kDefaultPeriod;
    }
    if (timing.qpcVBlank > 0) {
      // qpcVBlank: 最近一次 vblank 的 QPC。以"现在"为锚点换算, 并限制在 [now - period, now]。
      const int64_t delta_ticks =
          static_cast<int64_t>(timing.qpcVBlank) - static_cast<int64_t>(qpc_now.QuadPart);
      const TimeDelta delta = delta_ticks >= 0 ? QpcToDelta(delta_ticks, frequency.QuadPart)
                                               : -QpcToDelta(-delta_ticks, frequency.QuadPart);
      frame_start = std::clamp(now + delta, now - period, now);
    }
  }

  // 防空转: DwmFlush 失败(如合成不可用)或过早返回时, 补足睡眠到下一个周期。
  const bool early =
      shared->last_tick != TimePoint{} && now - shared->last_tick < period / 2;
  if (FAILED(flush) || early) {
    const TimePoint base = shared->last_tick != TimePoint{} ? shared->last_tick : now;
    SleepFor(base + period - now);
    now = Clock::now();
    frame_start = now;
  }
  shared->last_tick = now;
  const TimePoint frame_target = frame_start + period;

  {
    MutexLock lock(shared->lock);
    if (shared->destroyed || !shared->wanted) {
      // 空闲 tick: 停止(下一次 RequestFrameCallback 重新投递 Tick)。
      shared->running = false;
      return;
    }
    shared->wanted = false;
    // 回调在调用方(platform)线程执行: 只投递, 不在 vsync 线程调用。
    PostToObject(shared->owner, [frame_start, frame_target](WinVsyncDwm& self) {
      self.OnVsync(frame_start, frame_target);
    });
  }
  runner->PostTask([shared, runner] { Tick(shared, runner); });
}

void WinVsyncDwm::OnVsync(TimePoint frame_start, TimePoint frame_target) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (!callback_) {
    return;
  }
  // 先取走再调用: callback 里可以再次 RequestFrameCallback。
  Callback callback = std::move(callback_);
  callback_ = nullptr;
  callback(frame_start, frame_target);
}

}  // namespace momo::embedder::windows
