#pragma once

// MessagePump: EventLoop 的"等待与唤醒"策略(docs/08 §2)。EventLoop 管任务与对象生命周期,
// pump 只负责: 在合适的时刻回调 Delegate::DoWork(), 以及运行/退出底层循环。
//  - DefaultMessagePump: 条件变量 + 定时等待, 用于引擎自建线程(ui/raster/io)与测试。
//  - NativeMessagePump: 把 NativeLoop(平台层实现的 NSApp/GCD、Win32 消息泵)适配为 pump,
//    用于 OS 主线程。原生嵌套循环(模态框、live resize)期间 DoWork 可能被重入, 由 EventLoop 处理。
// 线程约定: Attach/Run/Quit 只在循环线程调用;ScheduleWork/ScheduleDelayedWork 任意线程。

#include <memory>

#include "base/time.h"

namespace momo {

class MessagePump {
 public:
  class Delegate {
   public:
    // 执行一轮迭代。返回下一次需要被回调的时刻;TimePoint::max() 表示等到下次 ScheduleWork。
    virtual TimePoint DoWork() = 0;

   protected:
    ~Delegate() = default;
  };

  virtual ~MessagePump() = default;

  // 由 EventLoop 构造时调用一次。delegate 比 pump 活得久。
  virtual void Attach(Delegate* delegate) = 0;
  // 在当前线程运行底层循环, 反复回调 DoWork, 直到 Quit。
  virtual void Run() = 0;
  // 在 DoWork 内部调用: 本次 DoWork 返回后 Run 尽快返回。
  virtual void Quit() = 0;
  // 请求尽快回调一次 DoWork。重复请求可合并。
  virtual void ScheduleWork() = 0;
  // 请求不晚于 when 回调 DoWork(允许提前与多余的回调)。
  virtual void ScheduleDelayedWork(TimePoint when) = 0;
};

std::unique_ptr<MessagePump> CreateDefaultMessagePump();

}  // namespace momo
