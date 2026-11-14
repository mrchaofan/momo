#include "embedder/windows/win_mouse_events.h"

#if defined(_WIN32)
#include "embedder/windows/win_util.h"
// 本文件的消息常量与 <windows.h> 必须一致。
static_assert(momo::embedder::windows::win32::kMouseMove == WM_MOUSEMOVE);
static_assert(momo::embedder::windows::win32::kLButtonDown == WM_LBUTTONDOWN);
static_assert(momo::embedder::windows::win32::kLButtonDblClk == WM_LBUTTONDBLCLK);
static_assert(momo::embedder::windows::win32::kRButtonDown == WM_RBUTTONDOWN);
static_assert(momo::embedder::windows::win32::kMButtonUp == WM_MBUTTONUP);
static_assert(momo::embedder::windows::win32::kMouseWheel == WM_MOUSEWHEEL);
static_assert(momo::embedder::windows::win32::kXButtonDown == WM_XBUTTONDOWN);
static_assert(momo::embedder::windows::win32::kXButtonDblClk == WM_XBUTTONDBLCLK);
static_assert(momo::embedder::windows::win32::kMouseHWheel == WM_MOUSEHWHEEL);
static_assert(momo::embedder::windows::win32::kMouseLeave == WM_MOUSELEAVE);
static_assert(momo::embedder::windows::win32::kMkLButton == MK_LBUTTON);
static_assert(momo::embedder::windows::win32::kMkShift == MK_SHIFT);
static_assert(momo::embedder::windows::win32::kMkXButton2 == MK_XBUTTON2);
static_assert(momo::embedder::windows::win32::kXButton1 == XBUTTON1);
static_assert(momo::embedder::windows::win32::kWheelDelta == WHEEL_DELTA);
static_assert(momo::embedder::windows::win32::kWheelPageScroll == WHEEL_PAGESCROLL);
#endif

namespace momo::embedder::windows {

using platform::PointerData;
using platform::PointerDeviceKind;

namespace {

uint16_t LowWord(uint64_t value) { return static_cast<uint16_t>(value & 0xFFFF); }
uint16_t HighWord(uint64_t value) { return static_cast<uint16_t>((value >> 16) & 0xFFFF); }

PointerDeviceKind KindFromExtraInfo(uint32_t extra) {
  if ((extra & win32::kPenSignatureMask) == win32::kPenSignature) {
    return (extra & win32::kTouchFlag) != 0 ? PointerDeviceKind::kTouch : PointerDeviceKind::kStylus;
  }
  return PointerDeviceKind::kMouse;
}

bool IsDown(uint32_t m) {
  return m == win32::kLButtonDown || m == win32::kRButtonDown || m == win32::kMButtonDown ||
         m == win32::kXButtonDown || m == win32::kLButtonDblClk || m == win32::kRButtonDblClk ||
         m == win32::kMButtonDblClk || m == win32::kXButtonDblClk;
}

bool IsUp(uint32_t m) {
  return m == win32::kLButtonUp || m == win32::kRButtonUp || m == win32::kMButtonUp || m == win32::kXButtonUp;
}

}  // namespace

int WinMouseMessage::click_count() const {
  switch (message) {
    case win32::kLButtonDblClk:
    case win32::kRButtonDblClk:
    case win32::kMButtonDblClk:
    case win32::kXButtonDblClk:
      return 2;
    default:
      return 1;
  }
}

bool IsWinMouseMessage(uint32_t message) {
  return (message >= win32::kMouseMove && message <= win32::kMouseHWheel) || message == win32::kMouseLeave;
}

uint32_t WinButtonBit(uint32_t message, uint64_t wparam) {
  switch (message) {
    case win32::kLButtonDown:
    case win32::kLButtonUp:
    case win32::kLButtonDblClk:
      return 1u << 0;
    case win32::kRButtonDown:
    case win32::kRButtonUp:
    case win32::kRButtonDblClk:
      return 1u << 1;
    case win32::kMButtonDown:
    case win32::kMButtonUp:
    case win32::kMButtonDblClk:
      return 1u << 2;
    case win32::kXButtonDown:
    case win32::kXButtonUp:
    case win32::kXButtonDblClk:
      // 侧键: XBUTTON1 = bit3, XBUTTON2 = bit4(与 macOS buttonNumber 3/4 一致)。
      return HighWord(wparam) == win32::kXButton1 ? (1u << 3) : (1u << 4);
    default:
      return 0;
  }
}

uint32_t WinHeldButtons(uint64_t wparam) {
  const uint32_t keys = LowWord(wparam);
  uint32_t buttons = 0;
  if (keys & win32::kMkLButton) buttons |= 1u << 0;
  if (keys & win32::kMkRButton) buttons |= 1u << 1;
  if (keys & win32::kMkMButton) buttons |= 1u << 2;
  if (keys & win32::kMkXButton1) buttons |= 1u << 3;
  if (keys & win32::kMkXButton2) buttons |= 1u << 4;
  return buttons;
}

void WinMouseInput::Dispatch(const WinMouseMessage& message, PointerWindow* window) {
  PointerInput input;
  if (Translate(message, window, input)) {
    dispatcher_.Dispatch(input);
  }
}

bool WinMouseInput::Translate(const WinMouseMessage& message, PointerWindow* window, PointerInput& input) {
  using Type = PointerInput::Type;
  const uint32_t m = message.message;
  if (!IsWinMouseMessage(m)) {
    return false;
  }
  input = PointerInput{};
  input.window = window;
  input.native = const_cast<WinMouseMessage*>(&message);
  PointerData& data = input.data;
  data.timestamp_us = message.timestamp_us;
  data.kind = KindFromExtraInfo(message.extra_info);
  data.screen_x = message.screen.x;
  data.screen_y = message.screen.y;
  if (m == win32::kMouseLeave) {
    input.type = Type::kLeave;
    return true;
  }
  if (m == win32::kMouseWheel || m == win32::kMouseHWheel) {
    input.type = Type::kScroll;
    const int delta = static_cast<int16_t>(HighWord(message.wparam));
    if (delta == 0) {
      return false;
    }
    // 精确触控板以 WHEEL_DELTA 的分数上报(Chromium 同样据此区分);普通滚轮总是整格。
    if (delta % win32::kWheelDelta != 0) {
      data.kind = PointerDeviceKind::kTrackpad;
    }
    const double notches = static_cast<double>(delta) / win32::kWheelDelta;
    bool horizontal = m == win32::kMouseHWheel;
    double points = 0;
    if (horizontal) {
      // 正 delta = 向右倾 = 视口向右滚(正 x)。
      points = notches * message.wheel_chars * kScrollLinePixels;
    } else {
      const double per_notch = message.wheel_lines == win32::kWheelPageScroll
                                   ? message.page_height
                                   : message.wheel_lines * kScrollLinePixels;
      // 正 delta = 向前滚(远离用户)= 视口向上滚: 取负, 统一为"正 y = 向下滚"(与 macOS / Web deltaY 一致)。
      points = -notches * per_notch;
      // Shift + 垂直滚轮 = 水平滚动(macOS 由系统完成, 这里对齐其行为)。
      horizontal = (LowWord(message.wparam) & win32::kMkShift) != 0;
    }
    if (points == 0) {
      return false;
    }
    if (horizontal) {
      data.scroll_delta_x = points;
    } else {
      data.scroll_delta_y = points;
    }
    return true;
  }
  const uint32_t held = WinHeldButtons(message.wparam);
  if (IsDown(m)) {
    input.type = Type::kDown;
    input.button = WinButtonBit(m, message.wparam);
    input.others_held = (held & ~input.button) != 0;  // 按下消息的按键状态已包含本键
    return true;
  }
  if (IsUp(m)) {
    input.type = Type::kUp;
    input.button = WinButtonBit(m, message.wparam);
    input.others_held = held != 0;  // 抬起消息的按键状态已不含本键
    return true;
  }
  // WM_MOUSEMOVE(拖动或悬停, 由派发器判断)。
  input.type = Type::kMove;
  input.others_held = held != 0;
  return true;
}

}  // namespace momo::embedder::windows
