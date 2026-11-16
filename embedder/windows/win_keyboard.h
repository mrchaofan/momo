#pragma once

// Windows 键盘消息 → platform::KeyData(docs/03 §3;对应 macOS 的 KeyDataFromKeyEvent)。
// 编码规则(physical_key / logical_key、特殊键)在 embedder/common/key_codes.h, 扫描码表在 win_key_mapping.h;
// 这里是依赖 Win32 的部分: 从消息里取扫描码、向系统要当前布局下的字符。
// 线程约定: platform(主)线程。

#include <cstdint>
#include <optional>

#include "embedder/common/engine_platform.h"
#include "embedder/windows/win_util.h"
#include "engine/platform/input_types.h"

namespace momo::embedder::windows {

// 此刻按住的修饰键(platform::kModifier* 位图)。
uint32_t CurrentKeyModifiers();

// WM_KEYDOWN / WM_SYSKEYDOWN / WM_KEYUP / WM_SYSKEYUP → KeyData。不对应一次物理按键的消息(SendInput 注入的
// Unicode 字符、键盘驱动合成的"假 Shift")为 nullopt。hwnd: 输入法处理中的按键要向它取原始虚拟键。
std::optional<platform::KeyData> KeyDataFromKeyMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

}  // namespace momo::embedder::windows
