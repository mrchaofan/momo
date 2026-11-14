#pragma once

// WinApp: PlatformApp 的 Win32 实现(应用级能力, 对应 macOS 的 MacApp)+ 应用级系统事件的汇集点。
// 仅被 embedder/windows 的 .cc 包含。
//
//  - 能力: Activate(把应用带到前台: 最前面的 momo 窗口还原并 SetForegroundWindow;steal = false 时只闪烁任务栏
//    按钮请求关注, 同 macOS requestUserAttention)。Windows 没有"隐藏整个应用"的概念: Hide/Show 不支持
//    (回复 unimplemented, 与 PlatformApp 的可选能力约定一致)。
//  - 事件: 每个顶层窗口都会收到 WM_ACTIVATEAPP, WinWindow 转给 OnActivateApp, 这里去重后交给 WinAppObserver
//    (入口的 WinAppDelegate, 它再交给 embedder::Application —— 同 macOS 的 NSApplicationDelegate)。
// 线程约定: 仅 platform 线程。

#include <optional>

#include "embedder/common/platform_app.h"

namespace momo::embedder::windows {

// 应用级系统事件的接收方(入口实现, 对应 macOS 的 NSApplicationDelegate)。
class WinAppObserver {
 public:
  // 应用成为/不再是前台应用(WM_ACTIVATEAPP)。
  virtual void OnAppActiveChanged(bool active) = 0;

 protected:
  ~WinAppObserver() = default;
};

class WinApp final : public PlatformApp {
 public:
  // observer 可为空;非空时须比本对象活得久。
  explicit WinApp(WinAppObserver* observer) : observer_(observer) {}

  uint32_t capabilities() const override { return kAppCapabilityActivate; }
  Status Activate(bool steal) override;

  // WM_ACTIVATEAPP(各顶层窗口都会收到同一次切换: 去重)。
  void OnActivateApp(bool active);

 private:
  WinAppObserver* observer_;
  std::optional<bool> active_;
};

}  // namespace momo::embedder::windows
