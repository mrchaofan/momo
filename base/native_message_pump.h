#pragma once

// NativeMessagePump: 把 NativeLoop(NSApp + GCD 主队列 / Win32 消息泵)适配为 MessagePump,
// 让 OS 主线程也运行统一的 EventLoop(docs/12 §3)。
//
// 唤醒 = 向 native 投递一个"执行 DoWork"的原生任务;延迟唤醒去重, 只保留最早的一个在途。
// 原生嵌套循环(模态框、live resize)期间 native 仍会执行这些任务, DoWork 因此可能重入,
// EventLoop 保证嵌套中不做延迟析构。
// 生命周期: native loop(由平台层拥有)必须比 pump 活得久;pump 析构后在途的原生任务变为空操作。
// 线程约定: 除 ScheduleWork/ScheduleDelayedWork 外仅 OS 主线程。

#include <memory>
#include <mutex>

#include "base/macros.h"
#include "base/message_pump.h"
#include "base/native_loop.h"

namespace momo {

class NativeMessagePump final : public MessagePump {
 public:
  explicit NativeMessagePump(NativeLoop& native);
  ~NativeMessagePump() override;

  void Attach(Delegate* delegate) override;
  void Run() override;
  void Quit() override;
  void ScheduleWork() override;
  void ScheduleDelayedWork(TimePoint when) override;

 private:
  // 在途原生任务共享的状态: pump 析构时置空 delegate。
  struct State {
    std::mutex mutex;
    Delegate* delegate = nullptr;               // 仅主线程读写
    TimePoint pending_delayed = TimePoint::max();  // 最早的在途延迟唤醒(任意线程, 加锁)
  };

  static void RunDoWork(const std::shared_ptr<State>& state, NativeLoop& native);
  static void ScheduleDelayed(const std::shared_ptr<State>& state,
                              NativeLoop& native, TimePoint when);

  NativeLoop& native_;
  std::shared_ptr<State> state_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(NativeMessagePump);
};

}  // namespace momo
