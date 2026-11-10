// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。

#include "embedder/windows/win_window.h"

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>
#include <winreg.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <utility>

#include "base/logging.h"
#include "embedder/windows/win_key_mapping.h"
#include "embedder/windows/win_surface_software.h"
#include "embedder/windows/win_text_input.h"

namespace momo::embedder::windows {

namespace {

constexpr wchar_t kWindowClass[] = L"MomoWindow";

// 普通滚轮每行对应的逻辑像素(与 mac / Chromium 一致)。一格(WHEEL_DELTA)= 系统"每次滚动行数"(默认 3)行。
constexpr double kScrollLinePixels = 40.0;

// DWMWA_USE_IMMERSIVE_DARK_MODE: Win10 20H1+ 为 20, 更早的 Win10 版本为 19(旧 SDK 无此枚举值)。
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmUseImmersiveDarkModeLegacy = 19;

// ToUnicodeEx 的 wFlags bit2: 不修改内核键盘状态(不吃掉死键), Win10 1607+。
constexpr UINT kToUnicodeNoStateChange = 0x4;

// 由触摸/笔模拟出的鼠标消息的 GetMessageExtraInfo 签名(MI_WP_SIGNATURE)。
constexpr DWORD kPenSignatureMask = 0xFFFFFF00;
constexpr DWORD kPenSignature = 0xFF515700;
constexpr DWORD kTouchFlag = 0x80;

LONG Round(double value) { return static_cast<LONG>(std::lround(value)); }

uint32_t CurrentModifiers() {
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

platform::PointerDeviceKind KindFromMessageExtraInfo() {
  const DWORD extra = static_cast<DWORD>(::GetMessageExtraInfo());
  if ((extra & kPenSignatureMask) == kPenSignature) {
    return (extra & kTouchFlag) != 0 ? platform::PointerDeviceKind::kTouch : platform::PointerDeviceKind::kStylus;
  }
  return platform::PointerDeviceKind::kMouse;
}

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

// 控制字符(C0/C1)与 DEL 不是"可打印字符"。
bool IsPrintable(uint32_t code_point) {
  return code_point >= 0x20 && code_point != 0x7F && !(code_point >= 0x80 && code_point < 0xA0);
}

bool AllPrintable(const std::wstring& text) {
  if (text.empty()) {
    return false;
  }
  for (wchar_t c : text) {
    const uint32_t unit = c;
    // 代理码元本身不是控制字符, 按可打印处理。
    if (unit < 0x20 || unit == 0x7F || (unit >= 0x80 && unit < 0xA0)) {
      return false;
    }
  }
  return true;
}

// ToUnicodeEx(不改变死键状态)。返回产生的字符串;死键返回其独立形式。
std::wstring TranslateKey(UINT virtual_key, UINT scan_code, const BYTE* key_state, HKL layout,
                          bool* dead_key) {
  wchar_t buffer[8] = {};
  const int count = ::ToUnicodeEx(virtual_key, scan_code, key_state, buffer,
                                  static_cast<int>(sizeof(buffer) / sizeof(buffer[0])),
                                  kToUnicodeNoStateChange, layout);
  *dead_key = count < 0;
  if (count == 0) {
    return {};
  }
  return std::wstring(buffer, count < 0 ? 1 : static_cast<size_t>(count));
}

// logical key: 规则同 mac(LogicalKeyFor)。
uint64_t LogicalKeyFor(UINT virtual_key, uint16_t scan_code, bool extended, uint32_t usage,
                       HKL layout) {
  if (IsSpecialHidUsage(usage)) {
    return SpecialLogicalKey(usage, scan_code, extended);
  }
  // 当前键盘布局下不带修饰键时的字符(Shift+1 → "1"), 小写。
  const BYTE empty_state[256] = {};
  bool dead_key = false;
  std::wstring base = TranslateKey(virtual_key, scan_code, empty_state, layout, &dead_key);
  if (!base.empty()) {
    ::CharLowerBuffW(base.data(), static_cast<DWORD>(base.size()));
  }
  const uint32_t code_point = FirstCodePoint(base);
  if (!IsPrintable(code_point)) {
    return SpecialLogicalKey(usage, scan_code, extended);
  }
  return code_point;
}

LPCWSTR CursorResource(platform::SystemCursor cursor) {
  switch (cursor) {
    case platform::SystemCursor::kArrow: return IDC_ARROW;
    case platform::SystemCursor::kIBeam: return IDC_IBEAM;
    case platform::SystemCursor::kPointingHand: return IDC_HAND;
    case platform::SystemCursor::kCrosshair: return IDC_CROSS;
    case platform::SystemCursor::kResizeLeftRight: return IDC_SIZEWE;
    case platform::SystemCursor::kResizeUpDown: return IDC_SIZENS;
    case platform::SystemCursor::kNotAllowed: return IDC_NO;
    case platform::SystemCursor::kHidden: return nullptr;
  }
  return IDC_ARROW;
}

}  // namespace

StatusOr<std::unique_ptr<WinWindow>> WinWindow::Create(const WindowSpec& spec,
                                                       WindowDelegate* delegate) {
  std::unique_ptr<WinWindow> window(new WinWindow(spec, delegate));
  MOMO_RETURN_IF_ERROR(window->Initialize(spec));
  return StatusOr<std::unique_ptr<WinWindow>>(std::move(window));
}

WinWindow::WinWindow(const WindowSpec& spec, WindowDelegate* delegate)
    : delegate_(delegate), resizable_(spec.resizable), min_size_(spec.min_size) {}

WinWindow::~WinWindow() {
  if (hwnd_ != nullptr) {
    // 先解除 WndProc 与本对象的关联: 销毁过程中的消息(WM_KILLFOCUS 等)不再触达 delegate。
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    text_input_.reset();  // 需要 HWND 仍有效(DestroyCaret 等)
    if (buttons_ != 0 && ::GetCapture() == hwnd_) {
      ::ReleaseCapture();
    }
    ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  surface_.reset();  // raster 已解绑(Shell::DetachView 先于窗口销毁);在途 present 消息随 HWND 丢弃
}

DWORD WinWindow::style() const {
  DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
  if (!resizable_) {
    style &= ~static_cast<DWORD>(WS_THICKFRAME | WS_MAXIMIZEBOX);
  }
  return style;
}

DWORD WinWindow::ex_style() const { return WS_EX_APPWINDOW; }

SIZE WinWindow::WindowSizeForClient(double logical_width, double logical_height, UINT dpi) const {
  const double scale = static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
  RECT rect = {0, 0, Round(logical_width * scale), Round(logical_height * scale)};
  ::AdjustWindowRectExForDpi(&rect, style(), FALSE, ex_style(), dpi);
  return {rect.right - rect.left, rect.bottom - rect.top};
}

Status WinWindow::Initialize(const WindowSpec& spec) {
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;  // 缩放时整窗重画(WM_PAINT 拉伸最新帧)
  wc.lpfnWndProc = &WinWindow::WndProc;
  wc.hInstance = instance;
  wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(1));  // exe 资源中的第一个图标(若有)
  if (wc.hIcon == nullptr) {
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
  }
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;  // 背景由 WM_PAINT 绘制(闪屏底色 / 最新帧)
  wc.lpszClassName = kWindowClass;
  if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return InternalError("RegisterClassExW failed: " + std::to_string(::GetLastError()));
  }

  // 先按系统 DPI 估算尺寸创建(不可见), 再按窗口实际所在显示器的 DPI 精确设置并居中。
  const std::wstring title = Utf8ToWide(spec.title);
  SIZE size = WindowSizeForClient(spec.size.width, spec.size.height, ::GetDpiForSystem());
  ::CreateWindowExW(ex_style(), kWindowClass, title.c_str(), style(), CW_USEDEFAULT, CW_USEDEFAULT,
                    size.cx, size.cy, nullptr, nullptr, instance, this);
  if (hwnd_ == nullptr) {  // WM_NCCREATE 中已赋值
    return InternalError("CreateWindowExW failed: " + std::to_string(::GetLastError()));
  }
  dpi_ = ::GetDpiForWindow(hwnd_);
  size = WindowSizeForClient(spec.size.width, spec.size.height, dpi_);
  HMONITOR monitor = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (::GetMonitorInfoW(monitor, &info)) {
    const RECT& work = info.rcWork;
    const LONG x = work.left + std::max<LONG>(0, (work.right - work.left - size.cx) / 2);
    const LONG y = work.top + std::max<LONG>(0, (work.bottom - work.top - size.cy) / 2);
    ::SetWindowPos(hwnd_, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
  } else {
    ::SetWindowPos(hwnd_, nullptr, 0, 0, size.cx, size.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  dpi_ = ::GetDpiForWindow(hwnd_);

  surface_ = std::make_unique<WinSoftwareSurface>(hwnd_, spec.background_argb);
  text_input_ = std::make_unique<WinTextInput>(hwnd_, delegate_);
  cursor_ = ::LoadCursorW(nullptr, IDC_ARROW);
  UpdateDarkMode();
  last_metrics_ = metrics();
  created_ = true;
  return Status::Ok();
}

void WinWindow::Show() {
  ::ShowWindow(hwnd_, SW_SHOWNORMAL);
  ::SetForegroundWindow(hwnd_);
  ::UpdateWindow(hwnd_);
}

void WinWindow::SetTitle(std::string_view title) {
  ::SetWindowTextW(hwnd_, Utf8ToWide(title).c_str());
}

void WinWindow::SetMinSize(Size logical_size) {
  min_size_ = logical_size;
  if (::IsIconic(hwnd_)) {
    return;
  }
  // 新的最小尺寸立即生效(WM_GETMINMAXINFO 只约束之后的拖拽)。
  RECT client{};
  ::GetClientRect(hwnd_, &client);
  const double dpr = device_pixel_ratio();
  const double width = (client.right - client.left) / dpr;
  const double height = (client.bottom - client.top) / dpr;
  if (width < logical_size.width || height < logical_size.height) {
    const SIZE size = WindowSizeForClient(std::max(width, logical_size.width),
                                          std::max(height, logical_size.height), dpi_);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, size.cx, size.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

Size WinWindow::physical_size() const {
  RECT client{};
  ::GetClientRect(hwnd_, &client);
  return {static_cast<double>(client.right - client.left),
          static_cast<double>(client.bottom - client.top)};
}

double WinWindow::device_pixel_ratio() const {
  return static_cast<double>(dpi_) / USER_DEFAULT_SCREEN_DPI;
}

platform::RenderSurface& WinWindow::surface() { return *surface_; }

platform::TextInputConnection& WinWindow::text_input() { return *text_input_; }

void WinWindow::SetCursor(platform::SystemCursor cursor) {
  cursor_hidden_ = cursor == platform::SystemCursor::kHidden;
  if (!cursor_hidden_) {
    cursor_ = ::LoadCursorW(nullptr, CursorResource(cursor));  // 系统共享光标, 无需释放
  }
  if (CursorInClient()) {
    ApplyCursor();
  }
}

bool WinWindow::CursorInClient() const {
  POINT point{};
  if (!::GetCursorPos(&point) || ::WindowFromPoint(point) != hwnd_) {
    return false;
  }
  ::ScreenToClient(hwnd_, &point);
  RECT client{};
  ::GetClientRect(hwnd_, &client);
  return ::PtInRect(&client, point) != FALSE;
}

void WinWindow::ApplyCursor() { ::SetCursor(cursor_hidden_ ? nullptr : cursor_); }

void WinWindow::NotifyMetricsChanged() {
  if (!created_ || minimized_) {
    return;
  }
  const platform::ViewportMetrics current = metrics();
  if (current == last_metrics_ || current.physical_size.IsEmpty()) {
    return;
  }
  last_metrics_ = current;
  delegate_->OnMetricsChanged(current);
}

void WinWindow::UpdateDarkMode() {
  DWORD light = 1;
  DWORD size = sizeof(light);
  const LSTATUS status = ::RegGetValueW(
      HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  const BOOL dark = (status == ERROR_SUCCESS && light == 0) ? TRUE : FALSE;
  if (FAILED(::DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark)))) {
    ::DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkModeLegacy, &dark, sizeof(dark));
  }
}

// ---------------------------------------------------------------------------
// 指针

platform::PointerData WinWindow::MakePointer(platform::PointerPhase phase, double x, double y) const {
  platform::PointerData data;
  data.timestamp_us = NowMicros();
  data.kind = last_kind_;
  data.phase = phase;
  data.device = 0;
  data.physical_x = x;  // Per-Monitor-V2: 客户区坐标即物理像素, 左上原点
  data.physical_y = y;
  data.buttons = buttons_;
  return data;
}

void WinWindow::DispatchPointer(platform::PointerData data) {
  platform::PointerDataPacket packet;
  if (data.phase == platform::PointerPhase::kRemove) {
    if (!pointer_added_) {
      return;
    }
    pointer_added_ = false;
  } else if (!pointer_added_) {
    // 首次出现(进入客户区或首个事件): 先补一个 kAdd(与 mac 一致)。
    platform::PointerData add = data;
    add.phase = platform::PointerPhase::kAdd;
    add.buttons = 0;
    add.scroll_delta_x = add.scroll_delta_y = 0;
    add.momentum = false;
    packet.push_back(add);
    pointer_added_ = true;
  }
  packet.push_back(data);
  delegate_->OnPointerPacket(std::move(packet));
}

void WinWindow::OnMouseMove(LPARAM lparam) {
  const double x = GET_X_LPARAM(lparam);
  const double y = GET_Y_LPARAM(lparam);
  last_kind_ = KindFromMessageExtraInfo();
  if (!tracking_leave_) {
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd_;
    tracking_leave_ = ::TrackMouseEvent(&track) != FALSE;
  }
  // 系统会发出无位移的 WM_MOUSEMOVE(激活、SetCursor 之后等): 去重。
  if (pointer_added_ && x == last_x_ && y == last_y_) {
    return;
  }
  last_x_ = x;
  last_y_ = y;
  DispatchPointer(MakePointer(buttons_ == 0 ? platform::PointerPhase::kHover : platform::PointerPhase::kMove, x, y));
}

void WinWindow::OnMouseLeave() {
  tracking_leave_ = false;
  if (buttons_ == 0) {
    // 拖拽中(捕获)离开不 remove, 松开时再补发(与 mac 一致)。
    DispatchPointer(MakePointer(platform::PointerPhase::kRemove, last_x_, last_y_));
  }
}

void WinWindow::OnMouseButton(UINT message, WPARAM wparam, LPARAM lparam) {
  uint32_t bit = 0;
  bool down = false;
  switch (message) {
    case WM_LBUTTONDOWN: bit = 1u << 0; down = true; break;
    case WM_LBUTTONUP: bit = 1u << 0; break;
    case WM_RBUTTONDOWN: bit = 1u << 1; down = true; break;
    case WM_RBUTTONUP: bit = 1u << 1; break;
    case WM_MBUTTONDOWN: bit = 1u << 2; down = true; break;
    case WM_MBUTTONUP: bit = 1u << 2; break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
      // 侧键: XBUTTON1 = bit3, XBUTTON2 = bit4(mac buttonNumber 3/4)。
      bit = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? (1u << 3) : (1u << 4);
      down = message == WM_XBUTTONDOWN;
      break;
    default:
      return;
  }
  const double x = GET_X_LPARAM(lparam);
  const double y = GET_Y_LPARAM(lparam);
  last_x_ = x;
  last_y_ = y;
  last_kind_ = KindFromMessageExtraInfo();
  if (down) {
    const bool first = buttons_ == 0;
    buttons_ |= bit;
    if (first) {
      ::SetCapture(hwnd_);  // 拖出窗口后仍收到移动/松开
    }
    DispatchPointer(MakePointer(first ? platform::PointerPhase::kDown : platform::PointerPhase::kMove, x, y));
    return;
  }
  if ((buttons_ & bit) == 0) {
    return;  // 按下发生在窗口外(如标题栏双击后)的松开
  }
  buttons_ &= ~bit;
  const bool last = buttons_ == 0;
  DispatchPointer(MakePointer(last ? platform::PointerPhase::kUp : platform::PointerPhase::kMove, x, y));
  if (last) {
    ::ReleaseCapture();  // 同步触发 WM_CAPTURECHANGED;此时 buttons_ 已为 0, OnCaptureLost 忽略
    if (!CursorInClient()) {
      // 拖出客户区后松开: 补发离开。
      DispatchPointer(MakePointer(platform::PointerPhase::kRemove, x, y));
    }
  }
}

void WinWindow::OnCaptureLost() {
  if (buttons_ == 0) {
    return;
  }
  // 按住期间被系统夺走捕获(Alt+Tab、弹窗等): 以松开结束本次拖拽, 避免按钮状态卡住。
  buttons_ = 0;
  DispatchPointer(MakePointer(platform::PointerPhase::kUp, last_x_, last_y_));
  if (!CursorInClient()) {
    DispatchPointer(MakePointer(platform::PointerPhase::kRemove, last_x_, last_y_));
  }
}

void WinWindow::OnMouseWheel(UINT message, WPARAM wparam, LPARAM lparam) {
  // 滚轮消息的坐标是屏幕坐标。
  POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  ::ScreenToClient(hwnd_, &point);
  const double notches = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
  const double dpr = device_pixel_ratio();
  platform::PointerData data = MakePointer(platform::PointerPhase::kScroll, point.x, point.y);
  data.kind = platform::PointerDeviceKind::kMouse;  // 精确触控板也以 WM_MOUSEWHEEL 小增量上报, 无法区分
  bool horizontal = message == WM_MOUSEHWHEEL;
  double pixels = 0;
  if (horizontal) {
    UINT chars = 3;
    ::SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);
    // 正 delta = 向右倾 = 视口向右滚(正 x)。
    pixels = notches * chars * kScrollLinePixels * dpr;
  } else {
    UINT lines = 3;
    ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const double per_notch = lines == WHEEL_PAGESCROLL ? physical_size().height
                                                       : lines * kScrollLinePixels * dpr;
    // 正 delta = 向前滚(远离用户)= 视口向上滚: 取负, 统一为"正 y = 向下滚"(与 mac / Web deltaY 一致)。
    pixels = -notches * per_notch;
    // Shift + 垂直滚轮 = 水平滚动(mac 由系统完成, 这里对齐其行为)。
    if ((::GetKeyState(VK_SHIFT) & 0x8000) != 0) {
      horizontal = true;
    }
  }
  if (horizontal) {
    data.scroll_delta_x = pixels;
  } else {
    data.scroll_delta_y = pixels;
  }
  if (data.scroll_delta_x == 0 && data.scroll_delta_y == 0) {
    return;
  }
  DispatchPointer(data);
}

// ---------------------------------------------------------------------------
// 键盘

void WinWindow::OnKey(UINT message, WPARAM wparam, LPARAM lparam) {
  const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
  UINT virtual_key = static_cast<UINT>(wparam);
  if (virtual_key == VK_PACKET) {
    return;  // SendInput 注入的 Unicode 字符: 无物理键, 字符经 WM_CHAR 到达
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
    return;  // 键盘驱动在 NumLock 下为方向键等合成的"假 Shift"
  }
  uint32_t usage = WinScanCodeToHidUsage(scan_code, extended);
  if (virtual_key == VK_NUMLOCK) {
    usage = 0x53;  // 扫描码 0x45 的扩展位在不同驱动上不可靠: 以虚拟键为准
  } else if (virtual_key == VK_PAUSE) {
    usage = 0x48;
  }

  // 输入法处理中的按键(VK_PROCESSKEY)取原始虚拟键计算 logical key;字符归输入法。
  const bool ime_key = virtual_key == VK_PROCESSKEY;
  const UINT logical_vk = ime_key ? ::ImmGetVirtualKey(hwnd_) : virtual_key;
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
  data.modifiers = CurrentModifiers();
  if (down && !ime_key) {
    // 当前修饰键状态下产生的字符(WM_CHAR 尚在队列中, 这里用 ToUnicodeEx 预先求值, 不改变死键状态)。
    BYTE key_state[256] = {};
    if (::GetKeyboardState(key_state)) {
      bool dead_key = false;
      const std::wstring characters =
          TranslateKey(virtual_key, scan_code, key_state, layout, &dead_key);
      if (!dead_key && AllPrintable(characters)) {
        data.character = WideToUtf8(characters);
      }
    }
  }
  // 编辑键(退格/方向键/回车等)只走 platform::KeyData, 不再发 kCommand(text_input_connection.h);
  // 可打印字符随后经 WM_CHAR → WinTextInput 以 kCommit 送达。
  delegate_->OnKeyData(data);
}

// ---------------------------------------------------------------------------
// WndProc

LRESULT CALLBACK WinWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    auto* window = static_cast<WinWindow*>(create->lpCreateParams);
    window->hwnd_ = hwnd;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
  }
  auto* self = reinterpret_cast<WinWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self == nullptr) {
    // WM_NCCREATE 之前的 WM_GETMINMAXINFO, 或析构开始之后的消息。
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
  }
  return self->HandleMessage(message, wparam, lparam);
}

LRESULT WinWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  if (text_input_ != nullptr) {
    LRESULT result = 0;
    if (text_input_->HandleImeMessage(message, wparam, lparam, &result)) {
      return result;
    }
  }
  switch (message) {
    // ---- 尺寸 / DPI ----
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      const SIZE min = WindowSizeForClient(min_size_.width, min_size_.height, dpi_);
      info->ptMinTrackSize.x = min.cx;
      info->ptMinTrackSize.y = min.cy;
      return 0;
    }
    case WM_GETDPISCALEDSIZE: {
      // 跨 DPI 拖动前询问新外框尺寸: 客户区按 DPI 比例精确缩放(默认的线性外框缩放有几像素误差)。
      const UINT new_dpi = static_cast<UINT>(wparam);
      RECT client{};
      ::GetClientRect(hwnd_, &client);
      const double scale = static_cast<double>(new_dpi) / static_cast<double>(dpi_);
      RECT rect = {0, 0, Round((client.right - client.left) * scale),
                   Round((client.bottom - client.top) * scale)};
      if (!::AdjustWindowRectExForDpi(&rect, style(), FALSE, ex_style(), new_dpi)) {
        return FALSE;
      }
      auto* size = reinterpret_cast<SIZE*>(lparam);
      size->cx = rect.right - rect.left;
      size->cy = rect.bottom - rect.top;
      return TRUE;
    }
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);  // 先更新: SetWindowPos 触发的 WM_SIZE 按新 DPR 上报
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      NotifyMetricsChanged();
      return 0;
    }
    case WM_SIZE:
      if (wparam == SIZE_MINIMIZED) {
        if (!minimized_) {
          minimized_ = true;
          if (created_) delegate_->OnVisibilityChanged(false);
        }
        return 0;
      }
      if (minimized_) {
        minimized_ = false;
        if (created_) delegate_->OnVisibilityChanged(true);
      }
      NotifyMetricsChanged();
      return 0;

    // ---- 绘制 ----
    case WM_ERASEBKGND:
      return 1;  // 背景在 WM_PAINT 中一次画完, 避免闪烁
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = ::BeginPaint(hwnd_, &paint);
      if (dc != nullptr) {
        if (surface_ != nullptr) {
          surface_->Paint(dc);
        }
        ::EndPaint(hwnd_, &paint);
      }
      return 0;
    }
    case WinSoftwareSurface::kPresentMessage:
      if (surface_ != nullptr) {
        surface_->DisplayPending();
      }
      return 0;

    // ---- 窗口生命周期 ----
    case WM_CLOSE:
      // embedder 总是拦截(返回 false), 询问 JS 后再异步销毁窗口;放行时只隐藏, HWND 由析构销毁。
      if (delegate_->OnCloseRequested()) {
        ::ShowWindow(hwnd_, SW_HIDE);
      }
      return 0;
    case WM_SETFOCUS:
      if (text_input_ != nullptr) text_input_->OnFocusChanged(true);
      if (created_) delegate_->OnFocusChanged(true);
      return 0;
    case WM_KILLFOCUS:
      if (text_input_ != nullptr) text_input_->OnFocusChanged(false);
      if (created_) delegate_->OnFocusChanged(false);
      return 0;
    case WM_SETTINGCHANGE:
      if (lparam != 0 &&
          std::wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
        UpdateDarkMode();
      }
      break;
    case WM_SYSCOMMAND:
      // 单独按 Alt / F10 / Alt+字母会进入菜单模式(本窗口无菜单, 只会吞掉下一次按键并蜂鸣)。
      // 保留 Alt+Space(系统菜单)。
      if ((wparam & 0xFFF0) == SC_KEYMENU && lparam != VK_SPACE) {
        return 0;
      }
      break;

    // ---- 光标 ----
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        ApplyCursor();
        return TRUE;
      }
      break;  // 非客户区(边框缩放光标等)交给系统

    // ---- 指针 ----
    case WM_MOUSEMOVE:
      OnMouseMove(lparam);
      return 0;
    case WM_MOUSELEAVE:
      OnMouseLeave();
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
      OnMouseButton(message, wparam, lparam);
      return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
      OnMouseButton(message, wparam, lparam);
      return TRUE;  // 文档要求 XBUTTON 消息返回 TRUE
    case WM_CAPTURECHANGED:
      OnCaptureLost();
      return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      OnMouseWheel(message, wparam, lparam);
      return 0;

    // ---- 键盘 ----
    case WM_KEYDOWN:
    case WM_KEYUP:
      OnKey(message, wparam, lparam);
      return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      OnKey(message, wparam, lparam);
      break;  // 交给 DefWindowProc: Alt+F4 → SC_CLOSE → WM_CLOSE 等系统行为
    case WM_CHAR:
      if (text_input_ != nullptr) {
        text_input_->OnChar(static_cast<wchar_t>(wparam));
      }
      return 0;

    default:
      break;
  }
  return ::DefWindowProcW(hwnd_, message, wparam, lparam);
}

}  // namespace momo::embedder::windows
