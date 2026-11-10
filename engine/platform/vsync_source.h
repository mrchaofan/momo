#pragma once

#include <functional>

#include "base/object.h"
#include "base/time.h"

namespace momo::engine::platform {

// 一次性 VSync 请求(docs/03 §4)。
// 生命周期(docs/08 §3): momo::Object, 归属创建它的 platform 线程;由 ObjectPtr 持有, 释放时 DeleteLater,
// 在 platform 线程的事件循环安全点析构 —— 可能晚于窗口销毁, 实现不得依赖窗口对象存活。
// 线程约定: RequestFrameCallback 与析构只在归属线程(platform)进行;回调也必须在该线程上执行
// (项目规则: 回调一律在调用方线程执行)。系统 vsync 线程(如 CVDisplayLink)只能投递, 不得直接调用回调。
// 析构返回后不得再触发回调。
class VsyncSource : public Object {
 public:
  using Callback = std::function<void(TimePoint frame_start, TimePoint frame_target)>;
  ~VsyncSource() override = default;
  virtual void RequestFrameCallback(Callback callback) = 0;

 protected:
  VsyncSource() = default;  // 归属当前线程的 EventLoop
};

}  // namespace momo::engine::platform
