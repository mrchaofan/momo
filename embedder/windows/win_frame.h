#pragma once

// Windows 窗口框架的几何(纯 C++, 不含 <windows.h>, 在任意平台单测)。WinWindow 的 WM_NCHITTEST、标题栏按钮的
// 布局与 window_controls_rect 都按这里计算, 全部使用内容区左上原点的逻辑点。
//
//   - 内侧缩放边框(WindowResizeOptions 的 border_*, 与 macOS 同一语义): 可缩放、未最大化/全屏时, 内容区边缘的这些
//     区域归系统缩放(光标显示缩放形状, 左键按下开始缩放), 优先于应用内容。上边只在隐藏标题栏时生效(有系统标题栏时
//     上边的缩放区在标题栏之外, 由系统处理)。
//   - 标题栏按钮(隐藏标题栏时由宿主绘制, win_caption_buttons.h): 最小化 / 最大化(还原) / 关闭, 靠内容区右上角,
//     高度 = 应用标题栏高度(TitleBarOptions::height, <= 0 时系统默认), 每个按钮宽 kCaptionButtonWidth。
//     窗口不可缩放时最大化按钮显示为停用。这块区域就是报告给应用的窗口控制按钮占用区域(WindowControlsOverlay)。
// 命中顺序: 缩放边框 → 标题栏按钮 → 内容(同 Windows 标准窗口: 右上角最外侧的像素用来缩放)。

#include <optional>

#include "base/geometry.h"
#include "embedder/common/window_types.h"

namespace momo::embedder::windows {

// Windows 10/11 标题栏按钮的标准宽度(逻辑点)。
inline constexpr double kCaptionButtonWidth = 46.0;

enum class WinFrameHit {
  kClient,
  kLeft,
  kRight,
  kTop,
  kBottom,
  kTopLeft,
  kTopRight,
  kBottomLeft,
  kBottomRight,
  kMinimizeButton,
  kMaximizeButton,
  kCloseButton,
  kDisabledButton,  // 停用的按钮(不可缩放窗口的最大化): 吞掉点击, 什么也不做
};

struct WinCaptionButtonsLayout {
  Rect bounds;      // 全部按钮(内容区坐标, 逻辑点);隐藏时为空
  Rect minimize;
  Rect maximize;
  Rect close;
  bool maximize_enabled = true;
  bool empty() const { return bounds.width <= 0 || bounds.height <= 0; }
  friend bool operator==(const WinCaptionButtonsLayout&, const WinCaptionButtonsLayout&) = default;
};

struct WinFrameState {
  Size client;                       // 内容区(逻辑点)
  WindowResizeOptions resize;
  bool maximized = false;
  bool fullscreen = false;
  bool title_bar_hidden = false;
  WinCaptionButtonsLayout buttons;   // title_bar_hidden 时有效
};

// 标题栏按钮的布局。client_width: 内容区宽;height: 按钮高(> 0)。
WinCaptionButtonsLayout LayoutCaptionButtons(double client_width, double height, bool maximize_enabled);

// 内容区坐标 point(逻辑点)处的命中。
WinFrameHit HitTestFrame(const WinFrameState& state, Point point);

// 报告给应用的窗口控制按钮占用区域: 标题栏未隐藏(按钮在内容区之外)或全屏时为 nullopt。
std::optional<Rect> WindowControlsRect(const WinFrameState& state);

}  // namespace momo::embedder::windows
