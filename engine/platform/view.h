#pragma once

// 视图(窗口)标识与创建参数(多视图, docs/02 §2)。
// 一个引擎实例(一个 JS 运行时)可以驱动多个视图: 每个视图对应宿主的一个原生窗口, 有自己的
// surface、尺寸、输入与焦点;帧时钟、JS 状态、平台视图表与纹理表在视图之间共享。
// 视图 0(kImplicitViewId)是宿主启动时创建的主窗口;其他视图由 JS 经 momo:ui createView 请求创建。

#include <cstdint>
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
};

}  // namespace momo::engine::platform
