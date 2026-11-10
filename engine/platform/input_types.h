#pragma once

// 平台无关输入事件(docs/03 §3)。坐标统一为物理像素。

#include <cstdint>
#include <string>
#include <vector>

namespace momo::engine::platform {

enum class PointerDeviceKind : uint8_t { kMouse, kTrackpad, kTouch, kStylus };
enum class PointerPhase : uint8_t { kAdd, kDown, kMove, kUp, kRemove, kHover, kScroll };

struct PointerData {
  int64_t timestamp_us = 0;
  PointerDeviceKind kind = PointerDeviceKind::kMouse;
  PointerPhase phase = PointerPhase::kHover;
  int32_t device = 0;
  double physical_x = 0;
  double physical_y = 0;
  uint32_t buttons = 0;  // 位图: bit0 主键, bit1 次键, bit2 中键
  double scroll_delta_x = 0;
  double scroll_delta_y = 0;
  bool momentum = false;  // Mac 惯性滚动阶段
  // 指针的屏幕位置(逻辑点, 主屏左上原点、y 向下;与窗口 screen_frame 同一坐标系)。
  // 跨窗口交互(拖出标签成窗)用它计算, 不受窗口自身移动的影响。
  double screen_x = 0;
  double screen_y = 0;
};

using PointerDataPacket = std::vector<PointerData>;

enum class KeyEventType : uint8_t { kDown, kUp, kRepeat };

enum KeyModifiers : uint32_t {
  kModifierNone = 0,
  kModifierShift = 1 << 0,
  kModifierControl = 1 << 1,
  kModifierAlt = 1 << 2,
  kModifierMeta = 1 << 3,
  kModifierCapsLock = 1 << 4,
};

struct KeyData {
  int64_t timestamp_us = 0;
  KeyEventType type = KeyEventType::kDown;
  uint64_t physical_key = 0;  // USB HID usage
  uint64_t logical_key = 0;
  std::string character;      // UTF-8, 可为空
  uint32_t modifiers = kModifierNone;
};

}  // namespace momo::engine::platform
