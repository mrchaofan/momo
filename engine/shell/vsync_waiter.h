#pragma once

// VsyncWaiter: Animator 使用的一次性 vsync 等待器(docs/05 §5)。回调总在 ui 线程触发。
//  - PlatformVsyncWaiter: 包装 platform::VsyncSource(Mac CVDisplayLink / Win DWM)。
//  - TimerVsyncWaiter:    平台无 vsync(或视图未绑定)时的回退, 在 ui 线程按固定刷新率对齐节拍。
// 线程约定: momo::Object, 归属 ui 线程, 仅在 ui 线程创建、调用与析构。析构后未触发的回调被丢弃。

#include <memory>

#include "base/closure.h"
#include "base/macros.h"
#include "base/object.h"
#include "base/task_runner.h"
#include "base/time.h"
#include "engine/platform/vsync_source.h"

namespace momo::engine {

// 刷新率未知时的默认值(Hz)。
inline constexpr double kDefaultRefreshRate = 60.0;

class VsyncWaiter : public Object {
 public:
  using Callback = MoveOnlyFunction<void(TimePoint frame_start, TimePoint frame_target)>;

  ~VsyncWaiter() override = default;

  // 请求下一次 vsync。回调前的重复请求被合并: 只回调最后一次传入的 callback, 且只回调一次。
  void AsyncWaitForVsync(Callback callback);
  bool is_waiting() const { return static_cast<bool>(callback_); }

 protected:
  explicit VsyncWaiter(std::shared_ptr<TaskRunner> ui_runner);

  // 子类向信号源请求一次 vsync;到达后(在 ui 线程)调用 FireCallback。
  virtual void AwaitVsync() = 0;
  void FireCallback(TimePoint frame_start, TimePoint frame_target);

  const std::shared_ptr<TaskRunner>& ui_runner() const { return task_runner(); }

 private:
  Callback callback_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(VsyncWaiter);
};

class TimerVsyncWaiter final : public VsyncWaiter {
 public:
  // refresh_rate <= 0 时按 kDefaultRefreshRate。节拍以构造时刻为相位对齐。
  TimerVsyncWaiter(std::shared_ptr<TaskRunner> ui_runner, double refresh_rate);

  TimeDelta interval() const { return interval_; }

 private:
  void AwaitVsync() override;

  TimeDelta interval_;
  TimePoint phase_;
};

class PlatformVsyncWaiter final : public VsyncWaiter {
 public:
  // source 归属 platform 线程(由 Shell 持有)。本 waiter 只持弱引用:
  // 每次请求投递到 source 的归属线程, 判活后再调用;source 已析构则本次请求作废
  // (视图解绑后 Shell 会把 Animator 切回定时器, 未完成的帧请求随之转移)。
  PlatformVsyncWaiter(std::shared_ptr<TaskRunner> ui_runner,
                      WeakPtr<platform::VsyncSource> source);

 private:
  void AwaitVsync() override;

  WeakPtr<platform::VsyncSource> source_;
};

}  // namespace momo::engine
