#pragma once

// macOS 键盘事件 → platform::KeyData 的映射(docs/03 §3)。仅被 .mm 文件包含。
//   physical_key: USB HID usage(Keyboard/Keypad page 0x07), 编码为 0x00070000 | usage;未知键为 0。
//   logical_key:  可打印键 = 不带修饰键时产生的字符(小写)的 Unicode 码点;
//                 特殊键(功能键、方向键、修饰键、回车/退格等)= kSpecialKeyBase + HID usage。

#include <cstdint>

namespace momo::embedder::macos {

inline constexpr uint64_t kHidKeyboardPage = 0x00070000;
inline constexpr uint64_t kSpecialKeyBase = 0x100000000;

// macOS virtual keycode(Carbon kVK_*)→ HID usage(0 = 未知)。
uint32_t MacKeyCodeToHidUsage(uint16_t key_code);

// HID usage → physical_key(0 → 0)。
inline uint64_t PhysicalKeyFromHidUsage(uint32_t usage) {
  return usage == 0 ? 0 : (kHidKeyboardPage | usage);
}

// 是否必须作为特殊键(不论其产生何种字符): 修饰键、回车、Tab、退格、Esc、功能键、方向键等。
bool IsSpecialKeyCode(uint16_t key_code);

// 修饰键的 keycode → 该键在 NSEventModifierFlags 设备相关位(NX_DEVICE*KEYMASK)中的位;非修饰键返回 0。
uint64_t DeviceModifierMaskForKeyCode(uint16_t key_code);

}  // namespace momo::embedder::macos
