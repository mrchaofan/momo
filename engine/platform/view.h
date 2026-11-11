#pragma once

// 视图(窗口)标识与创建参数(多视图, docs/02 §2)。
// 一个引擎实例(一个 JS 运行时)可以驱动多个视图: 每个视图对应宿主的一个原生窗口, 有自己的
// surface、尺寸、输入与焦点;帧时钟、JS 状态、平台视图表与纹理表在视图之间共享。
// 视图 0(kImplicitViewId)是宿主启动时创建的主窗口;其他视图由 JS 经 momo:ui createView 请求创建。

#include <cstdint>
#include <optional>
#include <string>

#include "base/geometry.h"

namespace momo::engine::platform {

using ViewId = int64_t;

inline constexpr ViewId kImplicitViewId = 0;

// JS 请求创建视图时的参数(逻辑像素)。
struct ViewOptions {
  std::string title;
  Size size{800, 600};
  Size min_size{200, 150};
  // 内容区左上角的屏幕位置(逻辑点, 主屏左上原点);为空时由宿主决定(居中)。
  std::optional<Point> position;
  // 面板(同 Electron 的 type: 'panel';macOS 为 NSPanel + NonactivatingPanel): 可成为 key window
  // 接收键盘输入, 但不成为 main window, 聚焦它也不激活应用——应用的其他窗口保持原状。
  bool panel = false;
  // 用户能否拖动边框缩放窗口(之后可经 view.setResizeOptions 修改)。
  bool resizable = true;
};

// 窗口的缩放行为(view.setResizeOptions)。默认值即常规桌面窗口的行为, 应用按需覆盖。
struct WindowResizeOptions {
  // 用户能否拖动边框缩放窗口(macOS: NSWindowStyleMaskResizable;绿色按钮的缩放随之启用/停用)。
  bool resizable = true;
  // 窗口内侧的系统缩放边框(逻辑点, 各边独立): 在这里光标显示为缩放形状、左键按下交给系统缩放,
  // 优先于拖动区域与应用内容。0 = 该边没有内侧边框——应用内容收到这里的全部输入, 系统在窗口外侧的
  // 缩放区仍可用(如顶栏全是可点击的标签页时把 top 设为 0)。
  double border_left = 5;
  double border_top = 5;
  double border_right = 5;
  double border_bottom = 5;
  // 拖拽缩放时主线程等待一帧新尺寸内容的上限(毫秒): 应用在此之内出帧则缩放无拉伸;0 = 不等待
  // (松手前内容按旧帧拉伸, 窗口拖动最跟手)。重绘很慢的应用可调大以换取无拉伸、调小以换取跟手。
  double live_resize_frame_wait_ms = 100;
};

}  // namespace momo::engine::platform
