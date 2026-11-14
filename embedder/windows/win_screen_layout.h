#pragma once

// WinScreenLayout: Windows 显示器的物理像素坐标 ↔ 对外的逻辑点坐标(纯 C++, 不含 <windows.h>, 在任意平台单测)。
//
// 对外约定(与 macOS 一致, embedder/common/display.h、PlatformWindow::screen_frame、PointerData::screen_x/y):
// 全局逻辑点坐标, 主屏左上原点、y 向下。Windows(Per-Monitor-V2)给出的是虚拟屏幕的物理像素, 各显示器 DPI 可以不同,
// 不能简单地整体除以一个缩放比。做法同 Chromium(display::win::ScreenWin):
//   - 每个显示器在逻辑空间里的大小 = 物理大小 / 自身 DPR;
//   - 主屏放在原点;其余显示器从主屏开始沿共享的边依次放置: 与已放置的显示器 P 相邻的显示器 M 紧贴 P 的对应边,
//     沿边方向的偏移按 P 的 DPR 换算——相邻关系与物理布局一致, 逻辑矩形之间不重叠(混合 DPI 下物理位置直接除以
//     DPR 会让右侧高 DPR 的显示器与主屏重叠)。
//   - 与任何显示器都不相邻的(有空隙的布局): 原点按主屏 DPR 换算。
// 点的换算: 物理 → 逻辑取点所在(或最近)的显示器;逻辑 → 物理取逻辑矩形包含该点(或最近)的显示器。
// 同一显示器内两者互逆, 所以指针的屏幕坐标与窗口的屏幕框在逻辑空间里可以直接相减。
// 线程约定: 不可变值对象, 任意线程。

#include <cstdint>
#include <vector>

#include "base/geometry.h"

namespace momo::embedder::windows {

// 一个显示器(物理像素, 虚拟屏幕坐标)。
struct WinMonitorInfo {
  uint64_t id = 0;
  Rect physical;       // 显示器范围
  Rect physical_work;  // 工作区(去掉任务栏)
  double dpr = 1.0;    // DPI / 96
  bool primary = false;
  double refresh_rate = 60.0;
};

class WinScreenLayout {
 public:
  WinScreenLayout() = default;
  explicit WinScreenLayout(std::vector<WinMonitorInfo> monitors);

  struct Monitor {
    WinMonitorInfo info;
    Rect logical;  // 逻辑空间中的范围
  };

  // 主屏在第一个。
  const std::vector<Monitor>& monitors() const { return monitors_; }
  bool empty() const { return monitors_.empty(); }

  Point ToLogical(Point physical) const;
  Point ToPhysical(Point logical) const;
  // 物理矩形 → 逻辑矩形: 原点按点换算, 尺寸按 dpr 换算(窗口所在显示器的 DPR, 即窗口的 DPR)。
  Rect ToLogical(const Rect& physical, double dpr) const;
  // 显示器的工作区(逻辑)。
  Rect LogicalWorkArea(const Monitor& monitor) const;

  // 物理点所在(或最近)的显示器;没有显示器时为 nullptr。
  const Monitor* MonitorForPhysical(Point physical) const;
  const Monitor* MonitorForLogical(Point logical) const;

 private:
  std::vector<Monitor> monitors_;
};

}  // namespace momo::embedder::windows
