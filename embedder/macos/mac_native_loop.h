#pragma once

// 主线程任务投递: GCD 主队列(由主 CFRunLoop 在 common modes 下服务, 拖拽/resize 的
// event tracking 期间同样执行)+ NSApplication run/stop 作为消息循环。

#include "base/native_loop.h"

namespace momo::embedder::macos {

class MacNativeLoop final : public NativeLoop {
 public:
  void PostTask(Closure task) override;
  void PostDelayedTask(Closure task, TimeDelta delay) override;
  bool RunsTasksOnCurrentThread() const override;
  void Run() override;
  void Quit() override;
};

}  // namespace momo::embedder::macos
