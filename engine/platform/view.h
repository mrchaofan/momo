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
};

}  // namespace momo::engine::platform
