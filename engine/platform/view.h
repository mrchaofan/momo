#pragma once

// 视图(窗口)标识(多视图, docs/02 §2)。
// 一个引擎实例(一个 JS 运行时)可以驱动多个视图: 每个视图对应宿主的一个原生窗口, 有自己的
// surface、尺寸、输入与焦点;帧时钟、JS 状态、平台视图表与纹理表在视图之间共享。
// 引擎只认识视图本身(尺寸、输入、画面);窗口的外观与行为(标题、位置、标题栏…)属于宿主,
// 经宿主注册的 momo/window 通道与 JS 交互, 不经过引擎。
// 视图 0(kImplicitViewId)是宿主启动时创建的窗口(隐式视图, 同 Flutter 的 implicit view): 它在 JS bundle
// 执行前就已存在, 供 runApp 直接渲染;除此之外与其他视图没有区别, 同样可以被关闭。

#include <cstdint>

namespace momo::engine::platform {

using ViewId = int64_t;

inline constexpr ViewId kImplicitViewId = 0;

}  // namespace momo::engine::platform
