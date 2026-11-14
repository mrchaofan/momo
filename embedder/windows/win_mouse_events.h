#pragma once

// Windows 的鼠标与其他位置型输入(docs/05 §9 "输入";对应 macOS 的 mac_mouse_events.h)。
// 纯 C++, 不含 <windows.h>: 消息常量在本头文件里按 Win32 的值定义(Windows 上编译时与 <windows.h> 静态核对),
// 翻译逻辑因此可以在任意平台单测。
//   - WinMouseMessage: 一条原生鼠标消息及其上下文(屏幕位置、系统设置等由窗口在消息到达时取好)。
//   - WinMouseInput: 进程内一个(由 WinPlatformIntegration 持有, 注入每个 WinWindow)。持有平台无关的
//     PointerDispatcher, 把 WinMouseMessage 翻译成 PointerInput 交给它。原生捕获(SetCapture)由窗口按派发器的
//     序列状态维护(win_window.cc"指针"):序列期间系统把鼠标消息都交给捕获窗口, 不需要 macOS 那样的进程级监视器。
// 线程约定: 只在主(platform)线程使用。

#include <cstdint>

#include "base/geometry.h"
#include "base/macros.h"
#include "embedder/common/pointer_dispatcher.h"

namespace momo::embedder::windows {

// Win32 消息与标志(值与 <winuser.h> 相同)。
namespace win32 {
inline constexpr uint32_t kMouseMove = 0x0200;      // WM_MOUSEMOVE
inline constexpr uint32_t kLButtonDown = 0x0201;    // WM_LBUTTONDOWN
inline constexpr uint32_t kLButtonUp = 0x0202;
inline constexpr uint32_t kLButtonDblClk = 0x0203;
inline constexpr uint32_t kRButtonDown = 0x0204;
inline constexpr uint32_t kRButtonUp = 0x0205;
inline constexpr uint32_t kRButtonDblClk = 0x0206;
inline constexpr uint32_t kMButtonDown = 0x0207;
inline constexpr uint32_t kMButtonUp = 0x0208;
inline constexpr uint32_t kMButtonDblClk = 0x0209;
inline constexpr uint32_t kMouseWheel = 0x020A;     // WM_MOUSEWHEEL
inline constexpr uint32_t kXButtonDown = 0x020B;
inline constexpr uint32_t kXButtonUp = 0x020C;
inline constexpr uint32_t kXButtonDblClk = 0x020D;
inline constexpr uint32_t kMouseHWheel = 0x020E;    // WM_MOUSEHWHEEL
inline constexpr uint32_t kMouseLeave = 0x02A3;     // WM_MOUSELEAVE

inline constexpr uint32_t kMkLButton = 0x0001;      // MK_LBUTTON
inline constexpr uint32_t kMkRButton = 0x0002;
inline constexpr uint32_t kMkShift = 0x0004;
inline constexpr uint32_t kMkControl = 0x0008;
inline constexpr uint32_t kMkMButton = 0x0010;
inline constexpr uint32_t kMkXButton1 = 0x0020;
inline constexpr uint32_t kMkXButton2 = 0x0040;
inline constexpr uint32_t kXButton1 = 0x0001;       // XBUTTON1(GET_XBUTTON_WPARAM)
inline constexpr int kWheelDelta = 120;             // WHEEL_DELTA
inline constexpr uint32_t kWheelPageScroll = 0xFFFFFFFF;  // WHEEL_PAGESCROLL

// GetMessageExtraInfo 的签名: 由触摸/笔模拟出的鼠标消息(MI_WP_SIGNATURE)。
inline constexpr uint32_t kPenSignatureMask = 0xFFFFFF00;
inline constexpr uint32_t kPenSignature = 0xFF515700;
inline constexpr uint32_t kTouchFlag = 0x80;
}  // namespace win32

// 一条原生鼠标消息。由窗口在 WndProc 里填好(屏幕位置取消息产生时的位置 GetMessagePos, 已换算为逻辑点)。
struct WinMouseMessage {
  uint32_t message = 0;
  uint64_t wparam = 0;
  Point screen;                // 逻辑点, 主屏左上原点(WinScreenLayout)
  int64_t timestamp_us = 0;
  uint32_t extra_info = 0;     // GetMessageExtraInfo()
  // 滚轮: 系统"每格滚动行数 / 字符数"(SPI_GETWHEELSCROLLLINES / CHARS);lines = kWheelPageScroll 为整页。
  uint32_t wheel_lines = 3;
  uint32_t wheel_chars = 3;
  double page_height = 0;      // 整页滚动时的位移(逻辑点, 窗口内容区高度)
  // 双击消息(WM_*BUTTONDBLCLK)视为第二次按下;序列的 click_count 供 startDrag 判断双击标题栏。
  int click_count() const;
};

// 滚轮每行对应的逻辑点(与 macOS / Chromium 一致)。
inline constexpr double kScrollLinePixels = 40.0;

// 鼠标消息的分类(纯函数)。
bool IsWinMouseMessage(uint32_t message);
// WM_*BUTTON* 的逻辑按键位(bit0 主键, bit1 次键, bit2 中键, bit3/4 侧键);不是按键消息时为 0。
uint32_t WinButtonBit(uint32_t message, uint64_t wparam);
// wparam 的 MK_* 按键状态 → 逻辑按键位图。
uint32_t WinHeldButtons(uint64_t wparam);

class WinMouseInput {
 public:
  WinMouseInput() = default;

  PointerDispatcher& dispatcher() { return dispatcher_; }

  // 翻译 message 并交给派发器(window: 收到消息的 momo 窗口)。不是指针输入时什么也不做。
  void Dispatch(const WinMouseMessage& message, PointerWindow* window);

  // WinMouseMessage → PointerInput(只翻译, 不做决定)。返回 false: 不是指针输入。
  // input.native 指向 message(仅在本次派发期间有效)。
  static bool Translate(const WinMouseMessage& message, PointerWindow* window, PointerInput& input);

 private:
  PointerDispatcher dispatcher_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinMouseInput);
};

}  // namespace momo::embedder::windows
