#include "embedder/windows/win_key_mapping.h"

#include <cstddef>

namespace momo::embedder::windows {

namespace {

struct KeyMapping {
  uint16_t scan_code;  // 扩展键为 0xE0xx
  uint32_t usage;      // USB HID usage(page 0x07)
};

// 完整的 Windows Set-1 扫描码 → HID usage 表(参考 Chromium ui/events/keycodes/dom/dom_code_data.inc
// 的 win 列)。按 scan_code 升序, 二分查找。
constexpr KeyMapping kKeyMappings[] = {
    {0x0001, 0x29},  // Escape
    {0x0002, 0x1E},  // Digit1
    {0x0003, 0x1F},  // Digit2
    {0x0004, 0x20},  // Digit3
    {0x0005, 0x21},  // Digit4
    {0x0006, 0x22},  // Digit5
    {0x0007, 0x23},  // Digit6
    {0x0008, 0x24},  // Digit7
    {0x0009, 0x25},  // Digit8
    {0x000A, 0x26},  // Digit9
    {0x000B, 0x27},  // Digit0
    {0x000C, 0x2D},  // Minus
    {0x000D, 0x2E},  // Equal
    {0x000E, 0x2A},  // Backspace
    {0x000F, 0x2B},  // Tab
    {0x0010, 0x14},  // KeyQ
    {0x0011, 0x1A},  // KeyW
    {0x0012, 0x08},  // KeyE
    {0x0013, 0x15},  // KeyR
    {0x0014, 0x17},  // KeyT
    {0x0015, 0x1C},  // KeyY
    {0x0016, 0x18},  // KeyU
    {0x0017, 0x0C},  // KeyI
    {0x0018, 0x12},  // KeyO
    {0x0019, 0x13},  // KeyP
    {0x001A, 0x2F},  // BracketLeft
    {0x001B, 0x30},  // BracketRight
    {0x001C, 0x28},  // Enter
    {0x001D, 0xE0},  // ControlLeft
    {0x001E, 0x04},  // KeyA
    {0x001F, 0x16},  // KeyS
    {0x0020, 0x07},  // KeyD
    {0x0021, 0x09},  // KeyF
    {0x0022, 0x0A},  // KeyG
    {0x0023, 0x0B},  // KeyH
    {0x0024, 0x0D},  // KeyJ
    {0x0025, 0x0E},  // KeyK
    {0x0026, 0x0F},  // KeyL
    {0x0027, 0x33},  // Semicolon
    {0x0028, 0x34},  // Quote
    {0x0029, 0x35},  // Backquote
    {0x002A, 0xE1},  // ShiftLeft
    {0x002B, 0x31},  // Backslash(ISO 键盘上为 IntlHash 位置, 同 Chromium 归为 Backslash)
    {0x002C, 0x1D},  // KeyZ
    {0x002D, 0x1B},  // KeyX
    {0x002E, 0x06},  // KeyC
    {0x002F, 0x19},  // KeyV
    {0x0030, 0x05},  // KeyB
    {0x0031, 0x11},  // KeyN
    {0x0032, 0x10},  // KeyM
    {0x0033, 0x36},  // Comma
    {0x0034, 0x37},  // Period
    {0x0035, 0x38},  // Slash
    {0x0036, 0xE5},  // ShiftRight
    {0x0037, 0x55},  // NumpadMultiply
    {0x0038, 0xE2},  // AltLeft
    {0x0039, 0x2C},  // Space
    {0x003A, 0x39},  // CapsLock
    {0x003B, 0x3A},  // F1
    {0x003C, 0x3B},  // F2
    {0x003D, 0x3C},  // F3
    {0x003E, 0x3D},  // F4
    {0x003F, 0x3E},  // F5
    {0x0040, 0x3F},  // F6
    {0x0041, 0x40},  // F7
    {0x0042, 0x41},  // F8
    {0x0043, 0x42},  // F9
    {0x0044, 0x43},  // F10
    {0x0045, 0x48},  // Pause(Windows 以非扩展 0x45 上报 Pause;NumLock 为扩展 0xE045)
    {0x0046, 0x47},  // ScrollLock
    {0x0047, 0x5F},  // Numpad7
    {0x0048, 0x60},  // Numpad8
    {0x0049, 0x61},  // Numpad9
    {0x004A, 0x56},  // NumpadSubtract
    {0x004B, 0x5C},  // Numpad4
    {0x004C, 0x5D},  // Numpad5
    {0x004D, 0x5E},  // Numpad6
    {0x004E, 0x57},  // NumpadAdd
    {0x004F, 0x59},  // Numpad1
    {0x0050, 0x5A},  // Numpad2
    {0x0051, 0x5B},  // Numpad3
    {0x0052, 0x62},  // Numpad0
    {0x0053, 0x63},  // NumpadDecimal
    {0x0054, 0x46},  // PrintScreen(Alt+PrintScreen 时以 SysRq 0x54 上报)
    {0x0056, 0x64},  // IntlBackslash
    {0x0057, 0x44},  // F11
    {0x0058, 0x45},  // F12
    {0x0059, 0x67},  // NumpadEqual
    {0x0064, 0x68},  // F13
    {0x0065, 0x69},  // F14
    {0x0066, 0x6A},  // F15
    {0x0067, 0x6B},  // F16
    {0x0068, 0x6C},  // F17
    {0x0069, 0x6D},  // F18
    {0x006A, 0x6E},  // F19
    {0x006B, 0x6F},  // F20
    {0x006C, 0x70},  // F21
    {0x006D, 0x71},  // F22
    {0x006E, 0x72},  // F23
    {0x0070, 0x88},  // KanaMode
    {0x0071, 0x91},  // Lang2
    {0x0072, 0x90},  // Lang1
    {0x0073, 0x87},  // IntlRo
    {0x0076, 0x73},  // F24
    {0x0079, 0x8A},  // Convert
    {0x007B, 0x8B},  // NonConvert
    {0x007D, 0x89},  // IntlYen
    {0x007E, 0x85},  // NumpadComma
    {0xE01C, 0x58},  // NumpadEnter
    {0xE01D, 0xE4},  // ControlRight
    {0xE020, 0x7F},  // AudioVolumeMute(与 mac 一致归入 Keyboard page)
    {0xE02E, 0x81},  // AudioVolumeDown
    {0xE030, 0x80},  // AudioVolumeUp
    {0xE035, 0x54},  // NumpadDivide
    {0xE037, 0x46},  // PrintScreen
    {0xE038, 0xE6},  // AltRight
    {0xE045, 0x53},  // NumLock
    {0xE046, 0x48},  // Pause(Ctrl+Break)
    {0xE047, 0x4A},  // Home
    {0xE048, 0x52},  // ArrowUp
    {0xE049, 0x4B},  // PageUp
    {0xE04B, 0x50},  // ArrowLeft
    {0xE04D, 0x4F},  // ArrowRight
    {0xE04F, 0x4D},  // End
    {0xE050, 0x51},  // ArrowDown
    {0xE051, 0x4E},  // PageDown
    {0xE052, 0x49},  // Insert
    {0xE053, 0x4C},  // Delete
    {0xE05B, 0xE3},  // MetaLeft
    {0xE05C, 0xE7},  // MetaRight
    {0xE05D, 0x65},  // ContextMenu
    {0xE05E, 0x66},  // Power
};

}  // namespace

uint32_t WinScanCodeToHidUsage(uint16_t scan_code, bool extended) {
  const uint16_t key = static_cast<uint16_t>((scan_code & 0xFF) | (extended ? 0xE000 : 0));
  size_t lo = 0;
  size_t hi = sizeof(kKeyMappings) / sizeof(kKeyMappings[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (kKeyMappings[mid].scan_code == key) {
      return kKeyMappings[mid].usage;
    }
    if (kKeyMappings[mid].scan_code < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return 0;
}

bool IsSpecialHidUsage(uint32_t usage) {
  switch (usage) {
    case 0x28:  // Enter
    case 0x29:  // Escape
    case 0x2A:  // Backspace
    case 0x2B:  // Tab
    case 0x39:  // CapsLock
    case 0x53:  // NumLock(mac: KeypadClear)
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

}  // namespace momo::embedder::windows
