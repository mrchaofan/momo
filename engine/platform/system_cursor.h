#pragma once

// 系统光标形状(window.setCursor → 宿主窗口)。属于窗口属性, 不是平台服务。

namespace momo::engine::platform {

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

}  // namespace momo::engine::platform
