#pragma once

// 按键的平台无关规则(docs/03 §3): KeyData 的 physical_key / logical_key 怎么编码、哪些键算"特殊键"。
// 这是框架可见的契约, 同一个键在每个平台必须得到同一个答案, 所以只在这里定义一次;平台层只负责
// "原生键码 → HID usage"的表和"取当前布局下不带修饰键的字符"的系统调用。
//   physical_key: USB HID usage(Keyboard/Keypad page 0x07), 编码为 0x00070000 | usage;未知键为 0。
//   logical_key:  可打印键 = 当前布局下不带修饰键时产生的字符(小写)的 Unicode 码点;
//                 特殊键(功能键、方向键、修饰键、回车/退格等)= kSpecialKeyBase + HID usage。
// 纯 C++, 不依赖任何平台头。

#include <cstdint>

namespace momo::embedder {

inline constexpr uint64_t kHidKeyboardPage = 0x00070000;
inline constexpr uint64_t kSpecialKeyBase = 0x100000000;

// HID usage → physical_key(0 → 0)。
constexpr uint64_t PhysicalKeyFromHidUsage(uint32_t usage) {
  return usage == 0 ? 0 : (kHidKeyboardPage | usage);
}

// 是否必须作为特殊键(不论它产生什么字符)。
constexpr bool IsSpecialHidUsage(uint32_t usage) {
  switch (usage) {
    case 0x28:  // Enter
    case 0x29:  // Escape
    case 0x2A:  // Backspace
    case 0x2B:  // Tab
    case 0x39:  // CapsLock
    case 0x53:  // NumLock(Apple 键盘: KeypadClear)
    case 0x58:  // NumpadEnter
    case 0x88:  // KanaMode
    case 0x8A:  // Convert
    case 0x8B:  // NonConvert
      return true;
    default:
      break;
  }
  if (usage >= 0xE0 && usage <= 0xE7) {
    return true;  // 修饰键
  }
  // F1..F12、PrintScreen..ArrowUp(0x3A..0x52)、ContextMenu..F24..音量(0x65..0x81, 除 NumpadEqual)、
  // Lang1..Lang5(0x90..0x94)。
  return (usage >= 0x3A && usage <= 0x52) || (usage >= 0x65 && usage <= 0x81 && usage != 0x67) ||
         (usage >= 0x90 && usage <= 0x94);
}

// 控制字符(C0/C1)与 DEL 不是"可打印字符"。
constexpr bool IsPrintableCodePoint(uint32_t code_point) {
  return code_point >= 0x20 && code_point != 0x7F && !(code_point >= 0x80 && code_point < 0xA0);
}

// 特殊键的 logical key。没有 HID usage 的键仍给出唯一值(放在 usage 空间之外): native_code 是平台的原生键码
// (< 0x10000;macOS 的 virtual keycode, Windows 的扫描码加扩展位)。
constexpr uint64_t SpecialLogicalKey(uint32_t usage, uint32_t native_code) {
  return kSpecialKeyBase + (usage != 0 ? usage : (0x10000u + native_code));
}

// 一个按键的 logical key。base_code_point: 当前键盘布局下不带修饰键时产生的字符的第一个码点, 已转小写
// (Shift+1 → '1';没有字符为 0)。
constexpr uint64_t LogicalKey(uint32_t usage, uint32_t native_code, uint32_t base_code_point) {
  if (IsSpecialHidUsage(usage) || !IsPrintableCodePoint(base_code_point)) {
    return SpecialLogicalKey(usage, native_code);
  }
  return base_code_point;
}

}  // namespace momo::embedder
