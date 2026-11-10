#pragma once

// NativeLoop: OS 原生消息循环的最小原语(由平台层实现: Mac NSApp + GCD 主队列, Win 隐藏消息窗口 + GetMessage 泵)。
// base 只定义接口;NativeMessagePump 把它适配成 EventLoop 的 MessagePump(docs/12 §3)。
// 线程约定: PostTask/PostDelayedTask/RunsTasksOnCurrentThread/Quit 可在任意线程调用;
// Run 只能在 OS 主线程调用。

#include "base/closure.h"
#include "base/time.h"

namespace momo {

class NativeLoop {
 public:
  virtual ~NativeLoop() = default;

  virtual void PostTask(Closure task) = 0;
  virtual void PostDelayedTask(Closure task, TimeDelta delay) = 0;
  // 当前线程是否为 OS 主线程(消息循环线程)。在 Run 之前也必须返回正确结果。
  virtual bool RunsTasksOnCurrentThread() const = 0;

  // 运行原生消息循环, 直到 Quit 被调用。允许嵌套(模态框等), 每次 Quit 只结束最内层。
  virtual void Run() = 0;
  // 请求退出(最内层)消息循环。未运行时调用的语义不保证。
  virtual void Quit() = 0;
};

}  // namespace momo
