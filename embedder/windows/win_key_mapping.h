#pragma once

// Windows 键盘事件 → platform::KeyData 的映射(docs/03 §3)。与 mac_key_mapping.h 约定一致:
//   physical_key: USB HID usage(Keyboard/Keypad page 0x07), 编码为 0x00070000 | usage;未知键为 0。
//   logical_key:  可打印键 = 当前布局下不带修饰键时产生的字符(小写)的 Unicode 码点;
//                 特殊键(功能键、方向键、修饰键、回车/退格等)= kSpecialKeyBase + HID usage。
// 本头文件不依赖 <windows.h>(纯 C++, 可在任意平台单测)。
#include <cstdint>

namespace momo::embedder::windows {

inline constexpr uint64_t kHidKeyboardPage = 0x00070000;
inline constexpr uint64_t kSpecialKeyBase = 0x100000000;

// Set-1 扫描码(WM_KEYDOWN lParam bit16–23)+ 扩展键标志(bit24, 即 0xE0 前缀)→ HID usage(0 = 未知)。
uint32_t WinScanCodeToHidUsage(uint16_t scan_code, bool extended);

// HID usage → physical_key(0 → 0)。
inline uint64_t PhysicalKeyFromHidUsage(uint32_t usage) {
  return usage == 0 ? 0 : (kHidKeyboardPage | usage);
}

// 是否必须作为特殊键(不论其产生何种字符): 修饰键、回车、Tab、退格、Esc、功能键、方向键、
// 编辑区、NumLock、小键盘回车、菜单/音量、输入法切换键等。与 mac IsSpecialKeyCode 的分类一致。
bool IsSpecialHidUsage(uint32_t usage);

// 无 HID usage 的键的 logical key: 仍给出唯一值(放在 usage 空间之外, 与 mac 同构)。
inline uint64_t SpecialLogicalKey(uint32_t usage, uint16_t scan_code, bool extended) {
  return kSpecialKeyBase +
         (usage != 0 ? usage : (0x10000u + (extended ? 0xE000u : 0u) + scan_code));
}

}  // namespace momo::embedder::windows
