#include "embedder/macos/mac_key_mapping.h"

#include <cstddef>

namespace momo::embedder::macos {

namespace {

struct KeyMapping {
  uint16_t key_code;  // kVK_*
  uint32_t usage;     // USB HID usage(page 0x07)
};

// 完整的 macOS virtual keycode → HID usage 表(参考 Chromium keycode_converter_data)。
constexpr KeyMapping kKeyMappings[] = {
    {0x00, 0x04},  // kVK_ANSI_A            → KeyA
    {0x01, 0x16},  // kVK_ANSI_S            → KeyS
    {0x02, 0x07},  // kVK_ANSI_D            → KeyD
    {0x03, 0x09},  // kVK_ANSI_F            → KeyF
    {0x04, 0x0B},  // kVK_ANSI_H            → KeyH
    {0x05, 0x0A},  // kVK_ANSI_G            → KeyG
    {0x06, 0x1D},  // kVK_ANSI_Z            → KeyZ
    {0x07, 0x1B},  // kVK_ANSI_X            → KeyX
    {0x08, 0x06},  // kVK_ANSI_C            → KeyC
    {0x09, 0x19},  // kVK_ANSI_V            → KeyV
    {0x0A, 0x64},  // kVK_ISO_Section       → IntlBackslash
    {0x0B, 0x05},  // kVK_ANSI_B            → KeyB
    {0x0C, 0x14},  // kVK_ANSI_Q            → KeyQ
    {0x0D, 0x1A},  // kVK_ANSI_W            → KeyW
    {0x0E, 0x08},  // kVK_ANSI_E            → KeyE
    {0x0F, 0x15},  // kVK_ANSI_R            → KeyR
    {0x10, 0x1C},  // kVK_ANSI_Y            → KeyY
    {0x11, 0x17},  // kVK_ANSI_T            → KeyT
    {0x12, 0x1E},  // kVK_ANSI_1            → Digit1
    {0x13, 0x1F},  // kVK_ANSI_2            → Digit2
    {0x14, 0x20},  // kVK_ANSI_3            → Digit3
    {0x15, 0x21},  // kVK_ANSI_4            → Digit4
    {0x16, 0x23},  // kVK_ANSI_6            → Digit6
    {0x17, 0x22},  // kVK_ANSI_5            → Digit5
    {0x18, 0x2E},  // kVK_ANSI_Equal        → Equal
    {0x19, 0x26},  // kVK_ANSI_9            → Digit9
    {0x1A, 0x24},  // kVK_ANSI_7            → Digit7
    {0x1B, 0x2D},  // kVK_ANSI_Minus        → Minus
    {0x1C, 0x25},  // kVK_ANSI_8            → Digit8
    {0x1D, 0x27},  // kVK_ANSI_0            → Digit0
    {0x1E, 0x30},  // kVK_ANSI_RightBracket → BracketRight
    {0x1F, 0x12},  // kVK_ANSI_O            → KeyO
    {0x20, 0x18},  // kVK_ANSI_U            → KeyU
    {0x21, 0x2F},  // kVK_ANSI_LeftBracket  → BracketLeft
    {0x22, 0x0C},  // kVK_ANSI_I            → KeyI
    {0x23, 0x13},  // kVK_ANSI_P            → KeyP
    {0x24, 0x28},  // kVK_Return            → Enter
    {0x25, 0x0F},  // kVK_ANSI_L            → KeyL
    {0x26, 0x0D},  // kVK_ANSI_J            → KeyJ
    {0x27, 0x34},  // kVK_ANSI_Quote        → Quote
    {0x28, 0x0E},  // kVK_ANSI_K            → KeyK
    {0x29, 0x33},  // kVK_ANSI_Semicolon    → Semicolon
    {0x2A, 0x31},  // kVK_ANSI_Backslash    → Backslash
    {0x2B, 0x36},  // kVK_ANSI_Comma        → Comma
    {0x2C, 0x38},  // kVK_ANSI_Slash        → Slash
    {0x2D, 0x11},  // kVK_ANSI_N            → KeyN
    {0x2E, 0x10},  // kVK_ANSI_M            → KeyM
    {0x2F, 0x37},  // kVK_ANSI_Period       → Period
    {0x30, 0x2B},  // kVK_Tab               → Tab
    {0x31, 0x2C},  // kVK_Space             → Space
    {0x32, 0x35},  // kVK_ANSI_Grave        → Backquote
    {0x33, 0x2A},  // kVK_Delete            → Backspace
    {0x35, 0x29},  // kVK_Escape            → Escape
    {0x36, 0xE7},  // kVK_RightCommand      → MetaRight
    {0x37, 0xE3},  // kVK_Command           → MetaLeft
    {0x38, 0xE1},  // kVK_Shift             → ShiftLeft
    {0x39, 0x39},  // kVK_CapsLock          → CapsLock
    {0x3A, 0xE2},  // kVK_Option            → AltLeft
    {0x3B, 0xE0},  // kVK_Control           → ControlLeft
    {0x3C, 0xE5},  // kVK_RightShift        → ShiftRight
    {0x3D, 0xE6},  // kVK_RightOption       → AltRight
    {0x3E, 0xE4},  // kVK_RightControl      → ControlRight
    // 0x3F kVK_Function(Fn): 无 Keyboard page usage, 不上报。
    {0x40, 0x6C},  // kVK_F17               → F17
    {0x41, 0x63},  // kVK_ANSI_KeypadDecimal  → NumpadDecimal
    {0x43, 0x55},  // kVK_ANSI_KeypadMultiply → NumpadMultiply
    {0x45, 0x57},  // kVK_ANSI_KeypadPlus     → NumpadAdd
    {0x47, 0x53},  // kVK_ANSI_KeypadClear    → NumLock
    {0x48, 0x80},  // kVK_VolumeUp          → AudioVolumeUp
    {0x49, 0x81},  // kVK_VolumeDown        → AudioVolumeDown
    {0x4A, 0x7F},  // kVK_Mute              → AudioVolumeMute
    {0x4B, 0x54},  // kVK_ANSI_KeypadDivide → NumpadDivide
    {0x4C, 0x58},  // kVK_ANSI_KeypadEnter  → NumpadEnter
    {0x4E, 0x56},  // kVK_ANSI_KeypadMinus  → NumpadSubtract
    {0x4F, 0x6D},  // kVK_F18               → F18
    {0x50, 0x6E},  // kVK_F19               → F19
    {0x51, 0x67},  // kVK_ANSI_KeypadEquals → NumpadEqual
    {0x52, 0x62},  // kVK_ANSI_Keypad0      → Numpad0
    {0x53, 0x59},  // kVK_ANSI_Keypad1      → Numpad1
    {0x54, 0x5A},  // kVK_ANSI_Keypad2      → Numpad2
    {0x55, 0x5B},  // kVK_ANSI_Keypad3      → Numpad3
    {0x56, 0x5C},  // kVK_ANSI_Keypad4      → Numpad4
    {0x57, 0x5D},  // kVK_ANSI_Keypad5      → Numpad5
    {0x58, 0x5E},  // kVK_ANSI_Keypad6      → Numpad6
    {0x59, 0x5F},  // kVK_ANSI_Keypad7      → Numpad7
    {0x5A, 0x6F},  // kVK_F20               → F20
    {0x5B, 0x60},  // kVK_ANSI_Keypad8      → Numpad8
    {0x5C, 0x61},  // kVK_ANSI_Keypad9      → Numpad9
    {0x5D, 0x89},  // kVK_JIS_Yen           → IntlYen
    {0x5E, 0x87},  // kVK_JIS_Underscore    → IntlRo
    {0x5F, 0x85},  // kVK_JIS_KeypadComma   → NumpadComma
    {0x60, 0x3E},  // kVK_F5                → F5
    {0x61, 0x3F},  // kVK_F6                → F6
    {0x62, 0x40},  // kVK_F7                → F7
    {0x63, 0x3C},  // kVK_F3                → F3
    {0x64, 0x41},  // kVK_F8                → F8
    {0x65, 0x42},  // kVK_F9                → F9
    {0x66, 0x91},  // kVK_JIS_Eisu          → Lang2
    {0x67, 0x44},  // kVK_F11               → F11
    {0x68, 0x90},  // kVK_JIS_Kana          → Lang1
    {0x69, 0x68},  // kVK_F13               → F13
    {0x6A, 0x6B},  // kVK_F16               → F16
    {0x6B, 0x69},  // kVK_F14               → F14
    {0x6D, 0x43},  // kVK_F10               → F10
    {0x6E, 0x65},  // kVK_ContextualMenu    → ContextMenu
    {0x6F, 0x45},  // kVK_F12               → F12
    {0x71, 0x6A},  // kVK_F15               → F15
    {0x72, 0x49},  // kVK_Help              → Insert
    {0x73, 0x4A},  // kVK_Home              → Home
    {0x74, 0x4B},  // kVK_PageUp            → PageUp
    {0x75, 0x4C},  // kVK_ForwardDelete     → Delete
    {0x76, 0x3D},  // kVK_F4                → F4
    {0x77, 0x4D},  // kVK_End               → End
    {0x78, 0x3B},  // kVK_F2                → F2
    {0x79, 0x4E},  // kVK_PageDown          → PageDown
    {0x7A, 0x3A},  // kVK_F1                → F1
    {0x7B, 0x50},  // kVK_LeftArrow         → ArrowLeft
    {0x7C, 0x4F},  // kVK_RightArrow        → ArrowRight
    {0x7D, 0x51},  // kVK_DownArrow         → ArrowDown
    {0x7E, 0x52},  // kVK_UpArrow           → ArrowUp
};

}  // namespace

uint32_t MacKeyCodeToHidUsage(uint16_t key_code) {
  // 表按 key_code 升序: 二分查找。
  size_t lo = 0;
  size_t hi = sizeof(kKeyMappings) / sizeof(kKeyMappings[0]);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (kKeyMappings[mid].key_code == key_code) {
      return kKeyMappings[mid].usage;
    }
    if (kKeyMappings[mid].key_code < key_code) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return 0;
}

bool IsSpecialKeyCode(uint16_t key_code) {
  switch (key_code) {
    case 0x24:  // Return
    case 0x30:  // Tab
    case 0x33:  // Backspace
    case 0x35:  // Escape
    case 0x4C:  // KeypadEnter
    case 0x47:  // KeypadClear
      return true;
    default:
      break;
  }
  if (key_code >= 0x36 && key_code <= 0x3F) {
    return true;  // 修饰键 + Fn
  }
  const uint32_t usage = MacKeyCodeToHidUsage(key_code);
  // F1..F12、Insert..ArrowUp(0x3A..0x52)、F13..F24 / 菜单 / 音量(0x68..0x81)、Lang(0x90..)。
  return (usage >= 0x3A && usage <= 0x52) || (usage >= 0x65 && usage <= 0x81 && usage != 0x67) ||
         (usage >= 0x90 && usage <= 0x91);
}

uint64_t DeviceModifierMaskForKeyCode(uint16_t key_code) {
  // IOKit NX_DEVICE*KEYMASK(IOLLEvent.h)。
  switch (key_code) {
    case 0x3B: return 0x00000001;  // NX_DEVICELCTLKEYMASK
    case 0x38: return 0x00000002;  // NX_DEVICELSHIFTKEYMASK
    case 0x3C: return 0x00000004;  // NX_DEVICERSHIFTKEYMASK
    case 0x37: return 0x00000008;  // NX_DEVICELCMDKEYMASK
    case 0x36: return 0x00000010;  // NX_DEVICERCMDKEYMASK
    case 0x3A: return 0x00000020;  // NX_DEVICELALTKEYMASK
    case 0x3D: return 0x00000040;  // NX_DEVICERALTKEYMASK
    case 0x3E: return 0x00002000;  // NX_DEVICERCTLKEYMASK
    default: return 0;
  }
}

}  // namespace momo::embedder::macos
