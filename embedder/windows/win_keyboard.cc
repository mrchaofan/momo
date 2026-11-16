#include "embedder/windows/win_keyboard.h"

#include <imm.h>

#include <string>

#include "embedder/windows/win_key_mapping.h"

namespace momo::embedder::windows {

namespace {

// ToUnicodeEx 的 wFlags bit2: 不修改内核键盘状态(不吃掉死键), Win10 1607+。
constexpr UINT kToUnicodeNoStateChange = 0x4;

uint32_t FirstCodePoint(const std::wstring& text) {
  if (text.empty()) {
    return 0;
  }
  const uint32_t high = text[0];
  if (high >= 0xD800 && high <= 0xDBFF && text.size() > 1) {
    const uint32_t low = text[1];
    if (low >= 0xDC00 && low <= 0xDFFF) {
      return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
    }
  }
  return high;
}

bool AllPrintable(const std::wstring& text) {
  if (text.empty()) {
    return false;
  }
  for (wchar_t c : text) {
    const uint32_t unit = c;
    // 代理码元本身不是控制字符, 按可打印处理。
    if (!IsPrintableCodePoint(unit)) {
      return false;
    }
  }
  return true;
}

// ToUnicodeEx(不改变死键状态)。返回产生的字符串;死键返回其独立形式。
std::wstring TranslateKey(UINT virtual_key, UINT scan_code, const BYTE* key_state, HKL layout, bool* dead_key) {
  wchar_t buffer[8] = {};
  const int count = ::ToUnicodeEx(virtual_key, scan_code, key_state, buffer,
                                  static_cast<int>(sizeof(buffer) / sizeof(buffer[0])), kToUnicodeNoStateChange,
                                  layout);
  *dead_key = count < 0;
  if (count == 0) {
    return {};
  }
  return std::wstring(buffer, count < 0 ? 1 : static_cast<size_t>(count));
}

// logical key(规则见 common/key_codes.h): 这里只取当前键盘布局下不带修饰键时的字符(Shift+1 → "1"), 小写。
uint64_t LogicalKeyFor(UINT virtual_key, uint16_t scan_code, bool extended, uint32_t usage, HKL layout) {
  const BYTE empty_state[256] = {};
  bool dead_key = false;
  std::wstring base = TranslateKey(virtual_key, scan_code, empty_state, layout, &dead_key);
  if (!base.empty()) {
    ::CharLowerBuffW(base.data(), static_cast<DWORD>(base.size()));
  }
  return LogicalKey(usage, WinNativeKeyCode(scan_code, extended), FirstCodePoint(base));
}

}  // namespace

uint32_t CurrentKeyModifiers() {
  uint32_t modifiers = platform::kModifierNone;
  if (::GetKeyState(VK_SHIFT) & 0x8000) modifiers |= platform::kModifierShift;
  if (::GetKeyState(VK_CONTROL) & 0x8000) modifiers |= platform::kModifierControl;
  if (::GetKeyState(VK_MENU) & 0x8000) modifiers |= platform::kModifierAlt;
  if ((::GetKeyState(VK_LWIN) & 0x8000) || (::GetKeyState(VK_RWIN) & 0x8000)) {
    modifiers |= platform::kModifierMeta;
  }
  if (::GetKeyState(VK_CAPITAL) & 0x0001) modifiers |= platform::kModifierCapsLock;
  return modifiers;
}

std::optional<platform::KeyData> KeyDataFromKeyMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
  const UINT virtual_key = static_cast<UINT>(wparam);
  if (virtual_key == VK_PACKET) {
    return std::nullopt;  // SendInput 注入的 Unicode 字符: 无物理键, 字符经 WM_CHAR 到达
  }
  uint16_t scan_code = static_cast<uint16_t>((lparam >> 16) & 0xFF);
  bool extended = ((lparam >> 24) & 0x1) != 0;
  if (scan_code == 0) {
    // 部分注入/虚拟键盘事件不带扫描码: 由虚拟键反查(MAPVK_VK_TO_VSC_EX 对扩展键返回 0xE0xx)。
    const UINT mapped = ::MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC_EX);
    scan_code = static_cast<uint16_t>(mapped & 0xFF);
    extended = (mapped & 0xFF00) == 0xE000;
  }
  if (extended && (scan_code == 0x2A || scan_code == 0x36)) {
    return std::nullopt;  // 键盘驱动在 NumLock 下为方向键等合成的"假 Shift"
  }
  uint32_t usage = WinScanCodeToHidUsage(scan_code, extended);
  if (virtual_key == VK_NUMLOCK) {
    usage = 0x53;  // 扫描码 0x45 的扩展位在不同驱动上不可靠: 以虚拟键为准
  } else if (virtual_key == VK_PAUSE) {
    usage = 0x48;
  }

  // 输入法处理中的按键(VK_PROCESSKEY)取原始虚拟键计算 logical key;字符归输入法。
  const bool ime_key = virtual_key == VK_PROCESSKEY;
  const UINT logical_vk = ime_key ? ::ImmGetVirtualKey(hwnd) : virtual_key;
  HKL layout = ::GetKeyboardLayout(0);

  platform::KeyData data;
  data.timestamp_us = NowMicros();
  if (down) {
    data.type = (lparam & (1 << 30)) != 0 ? platform::KeyEventType::kRepeat : platform::KeyEventType::kDown;
  } else {
    data.type = platform::KeyEventType::kUp;
  }
  data.physical_key = PhysicalKeyFromHidUsage(usage);
  data.logical_key = LogicalKeyFor(logical_vk, scan_code, extended, usage, layout);
  data.modifiers = CurrentKeyModifiers();
  if (down && !ime_key) {
    // 当前修饰键状态下产生的字符(WM_CHAR 尚在队列中, 这里用 ToUnicodeEx 预先求值, 不改变死键状态)。
    BYTE key_state[256] = {};
    if (::GetKeyboardState(key_state)) {
      bool dead_key = false;
      const std::wstring characters = TranslateKey(virtual_key, scan_code, key_state, layout, &dead_key);
      if (!dead_key && AllPrintable(characters)) {
        data.character = WideToUtf8(characters);
      }
    }
  }
  return data;
}

}  // namespace momo::embedder::windows
