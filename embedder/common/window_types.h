#pragma once

// 窗口管理的数据类型(momo/window 通道, docs/03 §2)。embedder 的平台抽象: engine 不可见——窗口外观
// 与行为属于宿主, 引擎只认识视图(尺寸、输入、画面)。

#include <cstdint>
#include <optional>
#include <string>

#include "base/geometry.h"

namespace momo::embedder {

// 系统光标形状(momo/window setCursor)。
enum class SystemCursor {
  kArrow,
  kIBeam,
  kPointingHand,
  kCrosshair,
  kResizeLeftRight,
  kResizeUpDown,
  kNotAllowed,
  kHidden,
};

// 窗口的缩放行为(momo/window setResizable / setResizeBorder, 与当前值合并)。默认值即常规桌面窗口的行为, 应用按需覆盖。
struct WindowResizeOptions {
  // 用户能否拖动边框缩放窗口(macOS: NSWindowStyleMaskResizable;绿色按钮的缩放随之启用/停用)。
  bool resizable = true;
  // 窗口内侧的系统缩放边框(逻辑点, 各边独立): 在这里光标显示为缩放形状、左键按下交给系统缩放,
  // 优先于应用内容。0 = 该边没有内侧边框——应用内容收到这里的全部输入, 系统在窗口外侧的缩放区仍可用。
  double border_left = 5;
  double border_top = 5;
  double border_right = 5;
  double border_bottom = 5;
  friend bool operator==(const WindowResizeOptions&, const WindowResizeOptions&) = default;
};

// 标题栏外观(momo/window setTitleBar)。
struct TitleBarOptions {
  // 隐藏系统标题栏: 内容铺满整个窗口, 系统窗口控制按钮(macOS 红绿灯、Windows 最小化/最大化/关闭)
  // 浮在内容之上, 占用区域经 WindowControlsOverlay 报告给应用。
  bool hidden = false;
  // 应用自绘的标题栏高度(逻辑点, hidden 时有效): 宿主让系统控制按钮在这个高度内垂直居中。
  // <= 0 = 系统默认位置。
  double height = 0;
  friend bool operator==(const TitleBarOptions&, const TitleBarOptions&) = default;
};

// 窗口状态(momo/window stateChanged / getState)。
struct WindowStateFlags {
  bool visible = true;      // 已显示(Show/Hide);最小化的窗口仍算 visible
  bool minimized = false;
  bool maximized = false;
  bool fullscreen = false;
  // 内容是否可能被看到: 不可见时引擎不为该视图出帧。
  bool shown() const { return visible && !minimized; }
  friend bool operator==(const WindowStateFlags&, const WindowStateFlags&) = default;
};

// 窗口创建参数(momo/window create;逻辑点)。
struct WindowOptions {
  std::string title;
  Size size{800, 600};
  Size min_size{200, 150};
  // 内容区左上角的屏幕位置(逻辑点, 主屏左上原点);为空时由宿主决定(居中)。
  std::optional<Point> position;
  // 辅助窗口: 可接收键盘输入, 但不成为应用的主窗口, 聚焦它也不激活应用——应用的其他窗口保持原状
  // (macOS: NSPanel + NonactivatingPanel;不支持的平台按普通窗口处理)。
  bool non_activating = false;
  // 用户能否拖动边框缩放窗口(之后可经 momo/window setResizable 修改)。
  bool resizable = true;
  // 创建后立即显示。false: 窗口创建并绑定引擎但不显示, 第一帧呈现后宿主发 readyToShow, 应用再 show
  // (避免先看到空白窗口, 同 Electron 的 show: false + ready-to-show)。
  bool show = true;
};


}  // namespace momo::embedder
