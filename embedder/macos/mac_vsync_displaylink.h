#pragma once

// MacVsyncDisplayLink: 基于 CVDisplayLink 的 platform::VsyncSource(docs/03 §4)。仅被 .mm 包含。
//
// 按需运行: RequestFrameCallback 时启动 display link;下一个 tick 在 platform 线程触发一次性 callback;
// 某个 tick 到达时没有待处理请求就停止 display link——无动画时零唤醒、零功耗。
//
// 线程与生命周期(vsync_source.h 契约: 回调在调用方线程执行):
//   - momo::Object, 归属 platform 线程(创建线程);RequestFrameCallback、callback 的调用、启停、
//     显示器切换与析构都只在该线程。
//   - CVDisplayLink 线程只做一件事: 在锁内记下最新的帧时间, 若还没有在途的通知, 就 PostToObject 到
//     platform 线程(OnTick)。它从不调用 callback, 也不碰其他成员。
//   - 析构: 先在锁内置 destroyed, 再 CVDisplayLinkStop(从非回调线程调用时会等待在途回调结束);
//     已投递但未执行的 OnTick 因 WeakPtr 失效而作废——析构返回后不会再回调。
//   - 不依赖窗口存活: 只弱引用 NSWindow 并监听其 NSWindowDidChangeScreenNotification(观察者在析构时
//     移除), 窗口跨屏时 CVDisplayLinkSetCurrentCGDisplay 切换到对应显示器。

#include <CoreVideo/CVDisplayLink.h>

#include <memory>

#include "base/status.h"
#include "embedder/common/engine_platform.h"
#include "engine/platform/vsync_source.h"

#ifdef __OBJC__
@class NSWindow;
#endif

namespace momo::embedder::macos {

class MacVsyncDisplayLink final : public platform::VsyncSource {
 public:
  // window 可为 nil(使用主显示器)。须在 platform 线程(有当前 EventLoop)调用。
  static StatusOr<ObjectPtr<platform::VsyncSource>> Create(NSWindow* window);

  ~MacVsyncDisplayLink() override;

  void RequestFrameCallback(Callback callback) override;

  // 供 MakeObject 使用;请用 Create。
  MacVsyncDisplayLink(CVDisplayLinkRef link, NSWindow* window);

 private:
  struct Shared;

  // platform 线程: 处理 CVDisplayLink 投递来的 tick。有请求则调用, 否则停止 display link。
  void OnTick();
  void UpdateDisplay();

  CVDisplayLinkRef link_;
  std::unique_ptr<Shared> shared_;  // 与 CVDisplayLink 回调线程共享(回调 context)
  Callback callback_;               // 仅 platform 线程: 待触发的一次性回调
  bool running_ = false;            // 仅 platform 线程
  NSWindow* __weak window_ = nil;
  id screen_observer_ = nil;
};

}  // namespace momo::embedder::macos
