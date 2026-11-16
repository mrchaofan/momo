#pragma once

// macOS 键盘事件 → platform::KeyData 的翻译(ARCHITECTURE.md §3.4)。仅被 .mm 文件包含。
// physical_key / logical_key 的编码与"哪些键是特殊键"是平台无关的规则, 见 embedder/common/key_codes.h;
// 这里只有 macOS 的部分: virtual keycode → HID usage 的表、取字符的 AppKit 调用、修饰键事件。

#include <cstdint>
#include <vector>

#include "embedder/common/engine_platform.h"
#include "embedder/common/key_codes.h"
#include "engine/platform/input_types.h"

@class NSEvent;

namespace momo::embedder::macos {

// macOS virtual keycode(Carbon kVK_*)→ HID usage(0 = 未知)。
uint32_t MacKeyCodeToHidUsage(uint16_t key_code);

// 修饰键的 keycode → 该键在 NSEventModifierFlags 设备相关位(NX_DEVICE*KEYMASK)中的位;非修饰键返回 0。
uint64_t DeviceModifierMaskForKeyCode(uint16_t key_code);

// keyDown / keyUp → KeyData(type 为 kDown / kRepeat / kUp;按下时带可打印字符)。
platform::KeyData KeyDataFromKeyEvent(NSEvent* event, platform::KeyEventType type);
// flagsChanged → 修饰键的 KeyData: 通常一个(按下或抬起);CapsLock 每次切换只有一个事件, 合成按下 + 抬起两个;
// 无 HID usage 的键(Fn 等)为空。
std::vector<platform::KeyData> KeyDataFromFlagsChanged(NSEvent* event);

}  // namespace momo::embedder::macos
