#pragma once

// Windows 键盘事件 → platform::KeyData 的映射(docs/03 §3)。physical_key / logical_key 的编码与"哪些键是
// 特殊键"是平台无关的规则, 见 embedder/common/key_codes.h;这里只有 Windows 的部分: 扫描码 → HID usage 的表。
// 本头文件不依赖 <windows.h>(纯 C++, 可在任意平台单测)。
#include <cstdint>

#include "embedder/common/key_codes.h"

namespace momo::embedder::windows {

// Set-1 扫描码(WM_KEYDOWN lParam bit16–23)+ 扩展键标志(bit24, 即 0xE0 前缀)→ HID usage(0 = 未知)。
uint32_t WinScanCodeToHidUsage(uint16_t scan_code, bool extended);

// 没有 HID usage 的键在 SpecialLogicalKey / LogicalKey 里的原生键码: 扫描码加扩展位。
constexpr uint32_t WinNativeKeyCode(uint16_t scan_code, bool extended) {
  return (extended ? 0xE000u : 0u) + scan_code;
}

}  // namespace momo::embedder::windows
