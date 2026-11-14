#pragma once

// WinDisplays: 显示器枚举与屏幕坐标换算的缓存(Win32 侧;换算规则在纯 C++ 的 WinScreenLayout)。
// 仅被 embedder/windows 的 .cc 包含。进程内一个(由 WinPlatformIntegration 持有, 注入每个 WinWindow)。
// 显示器布局缓存: 窗口收到 WM_DISPLAYCHANGE / WM_DPICHANGED / 工作区变化(WM_SETTINGCHANGE SPI_SETWORKAREA)时
// 调用 Invalidate;另有 1 秒的有效期兜底(没有窗口在那块显示器上时, 改另一块显示器的缩放比例不会通知到我们)。
// 线程约定: 仅 platform 线程。

#include <optional>
#include <vector>

#include "base/time.h"
#include "embedder/common/display.h"
#include "embedder/windows/win_screen_layout.h"
#include "embedder/windows/win_util.h"

namespace momo::embedder::windows {

class WinDisplays {
 public:
  const WinScreenLayout& layout();
  void Invalidate() { layout_.reset(); }

  // 主显示器在第一个(Application 取其刷新率)。逻辑点, 主屏左上原点。
  std::vector<DisplayInfo> displays();

  // 物理像素(虚拟屏幕坐标)↔ 逻辑点。
  Point ToLogical(POINT physical) { return layout().ToLogical(Point{double(physical.x), double(physical.y)}); }
  POINT ToPhysical(Point logical);

  // 当前系统枚举到的显示器(物理像素)。
  static std::vector<WinMonitorInfo> Enumerate();

 private:
  std::optional<WinScreenLayout> layout_;
  TimePoint enumerated_at_{};
};

}  // namespace momo::embedder::windows
