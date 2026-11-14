#include "embedder/windows/win_window.h"

#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <timeapi.h>
#include <windowsx.h>
#include <winreg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>

#include "base/event_loop.h"
#include "base/logging.h"
#include "embedder/windows/win_app.h"
#include "embedder/windows/win_caption_buttons.h"
#include "embedder/windows/win_displays.h"
#include "embedder/windows/win_key_mapping.h"
#include "embedder/windows/win_surface_d3d.h"
#include "embedder/windows/win_surface_software.h"
#include "embedder/windows/win_text_input.h"

namespace momo::embedder::windows {

namespace {

constexpr wchar_t kWindowClass[] = L"MomoWindow";

// DWMWA_USE_IMMERSIVE_DARK_MODE: Win10 20H1+ 为 20, 更早的 Win10 版本为 19(旧 SDK 无此枚举值)。
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmUseImmersiveDarkModeLegacy = 19;

// ToUnicodeEx 的 wFlags bit2: 不修改内核键盘状态(不吃掉死键), Win10 1607+。
constexpr UINT kToUnicodeNoStateChange = 0x4;

// 拖拽缩放时等待新尺寸帧的上限: 应用在此之内出帧则缩放无拉伸, 否则暂时拉伸旧内容(拖动保持跟手)。
constexpr TimeDelta kLiveResizeFrameWait = std::chrono::milliseconds(100);

LONG Round(double value) { return static_cast<LONG>(std::lround(value)); }

bool SoftwareRenderingForced() {
  const char* value = std::getenv("MOMO_SOFTWARE_RENDERING");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

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

// logical key: 规则同 macOS(LogicalKeyFor)。
uint64_t LogicalKeyFor(UINT virtual_key, uint16_t scan_code, bool extended, uint32_t usage, HKL layout) {
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

LPCWSTR CursorResource(SystemCursor cursor) {
  switch (cursor) {
    case SystemCursor::kArrow: return IDC_ARROW;
    case SystemCursor::kIBeam: return IDC_IBEAM;
    case SystemCursor::kPointingHand: return IDC_HAND;
    case SystemCursor::kCrosshair: return IDC_CROSS;
    case SystemCursor::kResizeLeftRight: return IDC_SIZEWE;
    case SystemCursor::kResizeUpDown: return IDC_SIZENS;
    case SystemCursor::kNotAllowed: return IDC_NO;
    case SystemCursor::kHidden: return nullptr;
  }
  return IDC_ARROW;
}

LRESULT ToHitCode(WinFrameHit hit) {
  switch (hit) {
    case WinFrameHit::kClient: return HTCLIENT;
    case WinFrameHit::kLeft: return HTLEFT;
    case WinFrameHit::kRight: return HTRIGHT;
    case WinFrameHit::kTop: return HTTOP;
    case WinFrameHit::kBottom: return HTBOTTOM;
    case WinFrameHit::kTopLeft: return HTTOPLEFT;
    case WinFrameHit::kTopRight: return HTTOPRIGHT;
    case WinFrameHit::kBottomLeft: return HTBOTTOMLEFT;
    case WinFrameHit::kBottomRight: return HTBOTTOMRIGHT;
    case WinFrameHit::kMinimizeButton: return HTMINBUTTON;
    case WinFrameHit::kMaximizeButton: return HTMAXBUTTON;
    case WinFrameHit::kCloseButton: return HTCLOSE;
    case WinFrameHit::kDisabledButton: return HTNOWHERE;
  }
  return HTCLIENT;
}

WinFrameHit CaptionButtonFromHitCode(WPARAM code) {
  switch (code) {
    case HTMINBUTTON: return WinFrameHit::kMinimizeButton;
    case HTMAXBUTTON: return WinFrameHit::kMaximizeButton;
    case HTCLOSE: return WinFrameHit::kCloseButton;
    default: return WinFrameHit::kClient;
  }
}

LPARAM PointToLParam(POINT p) {
  return MAKELPARAM(static_cast<WORD>(static_cast<SHORT>(p.x)), static_cast<WORD>(static_cast<SHORT>(p.y)));
}

}  // namespace

// ---------------------------------------------------------------------------
// 创建与销毁

StatusOr<std::unique_ptr<WinWindow>> WinWindow::Create(const WindowSpec& spec, WindowDelegate* delegate,
                                                       const Context& context) {
  std::unique_ptr<WinWindow> window(new WinWindow(spec, delegate, context));
  MOMO_RETURN_IF_ERROR(window->Initialize(spec));
  return StatusOr<std::unique_ptr<WinWindow>>(std::move(window));
}

WinWindow::WinWindow(const WindowSpec& spec, WindowDelegate* delegate, const Context& context)
    : delegate_(delegate),
      mouse_(context.mouse),
      displays_(context.displays),
      app_(context.app),
      min_size_(spec.min_size) {
  resize_options_.resizable = spec.resizable;
}

WinWindow::~WinWindow() {
  dispatcher().RemoveWindow(this);
  if (in_size_move_) {
    ::timeEndPeriod(1);  // 缩放循环中被销毁: 配对 WM_ENTERSIZEMOVE 的 timeBeginPeriod
  }
  if (hwnd_ != nullptr) {
    // 先解除 WndProc 与本对象的关联: 销毁过程中的消息(WM_KILLFOCUS 等)不再触达 delegate。
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    text_input_.reset();  // 需要 HWND 仍有效(DestroyCaret 等)
    caption_buttons_.reset();
    if (capturing_ && ::GetCapture() == hwnd_) {
      ::ReleaseCapture();
    }
    // 交换链先于窗口释放(raster 已解绑: Shell::DetachView 先于窗口销毁)。
    surface_.reset();
    ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  surface_.reset();
}

WinWindow* WinWindow::FromHwnd(HWND hwnd) {
  wchar_t name[32] = {};
  if (hwnd == nullptr || ::GetClassNameW(hwnd, name, 32) == 0 || std::wcscmp(name, kWindowClass) != 0) {
    return nullptr;
  }
  // 别的 momo 进程的窗口也叫这个类名: 只认本进程的(GWLP_USERDATA 是本进程的指针)。
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ::GetCurrentProcessId()) {
    return nullptr;
  }
  return reinterpret_cast<WinWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

HWND WinWindow::FrontmostVisibleHwnd() {
  // 顶层窗口按 z 序由前到后。
  for (HWND hwnd = ::GetTopWindow(nullptr); hwnd != nullptr; hwnd = ::GetWindow(hwnd, GW_HWNDNEXT)) {
    if (::IsWindowVisible(hwnd) && FromHwnd(hwnd) != nullptr) {
      return hwnd;
    }
  }
  return nullptr;
}

DWORD WinWindow::style() const {
  // 不带 WS_CLIPCHILDREN: 唯一的子窗口是分层的控制按钮(WinCaptionButtons, 大部分透明), 软件 surface 的 GDI 绘制
  // 要画到它下面。
  DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS;
  if (!resize_options_.resizable) {
    style &= ~static_cast<DWORD>(WS_THICKFRAME | WS_MAXIMIZEBOX);
  }
  return style;
}

DWORD WinWindow::ex_style() const { return WS_EX_APPWINDOW; }

void WinWindow::ApplyStyle() {
  const LONG_PTR current = ::GetWindowLongPtrW(hwnd_, GWL_STYLE);
  const LONG_PTR kept = current & (WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE | WS_DISABLED);
  const LONG_PTR next = kept | static_cast<LONG_PTR>(style());
  if (next != current) {
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, next);
  }
  ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

SIZE WinWindow::WindowSizeForClient(double logical_width, double logical_height, UINT dpi) const {
  const double scale = static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
  RECT rect = {0, 0, Round(logical_width * scale), Round(logical_height * scale)};
  if (!title_bar_.hidden) {
    ::AdjustWindowRectExForDpi(&rect, style(), FALSE, ex_style(), dpi);
  }
  return {rect.right - rect.left, rect.bottom - rect.top};
}

Status WinWindow::Initialize(const WindowSpec& spec) {
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  // CS_DBLCLKS: 第二次按下以 WM_*BUTTONDBLCLK 到达(startDrag 据此识别双击标题栏)。
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
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

  // 先按系统 DPI 估算尺寸创建(不可见), 再按窗口实际所在显示器的 DPI 精确设置并定位。
  const std::wstring title = Utf8ToWide(spec.title);
  SIZE size = WindowSizeForClient(spec.size.width, spec.size.height, ::GetDpiForSystem());
  ::CreateWindowExW(ex_style(), kWindowClass, title.c_str(), style(), CW_USEDEFAULT, CW_USEDEFAULT, size.cx,
                    size.cy, nullptr, nullptr, instance, this);
  if (hwnd_ == nullptr) {  // WM_NCCREATE 中已赋值
    return InternalError("CreateWindowExW failed: " + std::to_string(::GetLastError()));
  }
  dpi_ = ::GetDpiForWindow(hwnd_);
  size = WindowSizeForClient(spec.size.width, spec.size.height, dpi_);
  HMONITOR monitor = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (spec.position) {
    ::SetWindowPos(hwnd_, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetPosition(*spec.position).IgnoreError();
  } else if (::GetMonitorInfoW(monitor, &info)) {
    const RECT& work = info.rcWork;
    const LONG x = work.left + std::max<LONG>(0, (work.right - work.left - size.cx) / 2);
    const LONG y = work.top + std::max<LONG>(0, (work.bottom - work.top - size.cy) / 2);
    ::SetWindowPos(hwnd_, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
  } else {
    ::SetWindowPos(hwnd_, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  dpi_ = ::GetDpiForWindow(hwnd_);

  CreateSurface(spec.background_argb);
  text_input_ = std::make_unique<WinTextInput>(hwnd_, delegate_);
  cursor_ = ::LoadCursorW(nullptr, IDC_ARROW);
  UpdateDarkMode();
  last_metrics_ = metrics();
  last_state_ = state();  // 尚未显示
  created_ = true;
  return Status::Ok();
}

void WinWindow::CreateSurface(uint32_t background_argb) {
  if (SoftwareRenderingForced()) {
    MOMO_LOG(INFO) << "MOMO_SOFTWARE_RENDERING set: using the software surface.";
  } else {
    StatusOr<std::unique_ptr<WinD3DSurface>> d3d = WinD3DSurface::Create(hwnd_, background_argb);
    if (d3d.ok()) {
      d3d_surface_ = d3d->get();
      software_surface_ = &d3d_surface_->software();
      surface_ = std::move(d3d).value();
      return;
    }
    MOMO_LOG(WARNING) << "Direct3D surface unavailable (" << d3d.status() << "); using the software surface.";
  }
  auto software = std::make_unique<WinSoftwareSurface>(hwnd_, background_argb);
  software_surface_ = software.get();
  surface_ = std::move(software);
}

// ---------------------------------------------------------------------------
// 显示与状态

void WinWindow::Show() {
  ::ShowWindow(hwnd_, ::IsZoomed(hwnd_) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
  ::SetForegroundWindow(hwnd_);
  ::UpdateWindow(hwnd_);
  NotifyStateChanged();
}

void WinWindow::Activate() {
  if (::IsIconic(hwnd_)) {
    ::ShowWindow(hwnd_, SW_RESTORE);
  }
  Show();
}

WindowStateFlags WinWindow::state() const {
  WindowStateFlags s;
  s.minimized = ::IsIconic(hwnd_) != FALSE;
  s.visible = ::IsWindowVisible(hwnd_) != FALSE;
  s.maximized = !s.minimized && ::IsZoomed(hwnd_) != FALSE;
  return s;
}

void WinWindow::NotifyStateChanged() {
  if (!created_) return;
  const WindowStateFlags s = state();
  if (s == last_state_) return;
  last_state_ = s;
  delegate_->OnWindowStateChanged(s);
}

uint32_t WinWindow::capabilities() const {
  // 平台视图与非激活窗口不支持(见头文件)。
  return kCapabilityAlwaysOnTop | kCapabilityPosition | kCapabilityResizeOptions | kCapabilityHiddenTitleBar |
         kCapabilityWindowDrag | kCapabilityMoveLoop | kCapabilityPointerCapture | kCapabilityVisibility |
         kCapabilityMinMax;
}

Status WinWindow::Hide() {
  ::ShowWindow(hwnd_, SW_HIDE);
  NotifyStateChanged();
  return Status::Ok();
}

Status WinWindow::Minimize() {
  ::ShowWindow(hwnd_, SW_MINIMIZE);
  NotifyStateChanged();
  return Status::Ok();
}

Status WinWindow::Maximize() {
  ::ShowWindow(hwnd_, SW_MAXIMIZE);
  NotifyStateChanged();
  return Status::Ok();
}

Status WinWindow::Restore() {
  ::ShowWindow(hwnd_, SW_RESTORE);
  NotifyStateChanged();
  return Status::Ok();
}

Status WinWindow::SetAlwaysOnTop(bool on) {
  ::SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  return Status::Ok();
}

void WinWindow::SetTitle(std::string_view title) { ::SetWindowTextW(hwnd_, Utf8ToWide(title).c_str()); }

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
    ::SetWindowPos(hwnd_, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

Size WinWindow::physical_size() const {
  RECT client{};
  ::GetClientRect(hwnd_, &client);
  return {static_cast<double>(client.right - client.left), static_cast<double>(client.bottom - client.top)};
}

double WinWindow::device_pixel_ratio() const { return static_cast<double>(dpi_) / USER_DEFAULT_SCREEN_DPI; }

platform::RenderSurface& WinWindow::surface() { return *surface_; }

TextInputConnection& WinWindow::text_input() { return *text_input_; }

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

void WinWindow::SynchronizeLiveResize() {
  // 嵌套(等待期间执行的平台任务又改变了窗口尺寸)时不再等待, 由外层的等待覆盖;还没出过帧时等待只会超时。
  const bool presented = d3d_surface_ != nullptr ? d3d_surface_->has_presented() : software_surface_->has_presented();
  if (resize_waiting_ || !presented) {
    NotifyMetricsChanged();
    return;
  }
  resize_waiting_ = true;
  // 先把新尺寸交给引擎, 再等它画出这个尺寸的帧(WM_SIZE 返回后系统才显示新尺寸的窗口)。
  NotifyMetricsChanged();
  EventLoop* loop = EventLoop::Current();
  const std::weak_ptr<int> alive = alive_;
  // 执行平台线程任务(vsync 回调等: 引擎出下一帧要经过 platform 线程)。不派发窗口消息(包括别的线程或进程发来的
  // 同步消息, 如 WM_ENDSESSION), 尽量少重入;等待有上限, 不会与 raster 线程永久互等。返回本窗口是否仍然存在。
  const auto pump = [loop, alive] {
    if (loop != nullptr) {
      loop->RunDueTasksNested();
    }
    return !alive.expired();
  };
  const bool got_frame = d3d_surface_ != nullptr
                             ? d3d_surface_->WaitForFrame(physical_size(), kLiveResizeFrameWait, pump)
                             : software_surface_->WaitForFrame(physical_size(), kLiveResizeFrameWait, pump);
  if (alive.expired()) {
    return;  // 等待期间窗口被关闭(有序关闭等): 不再访问本对象
  }
  if (!got_frame) {
    MOMO_LOG(VERBOSE) << "Live resize: no frame of the new size within the timeout; content is stretched.";
  }
  resize_waiting_ = false;
}

void WinWindow::UpdateDarkMode() {
  DWORD light = 1;
  DWORD size = sizeof(light);
  const LSTATUS status =
      ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  dark_ = status == ERROR_SUCCESS && light == 0;
  const BOOL dark = dark_ ? TRUE : FALSE;
  if (FAILED(::DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark)))) {
    ::DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkModeLegacy, &dark, sizeof(dark));
  }
  UpdateCaptionButtons();
}

// ---------------------------------------------------------------------------
// 光标

void WinWindow::SetCursor(SystemCursor cursor) {
  cursor_hidden_ = cursor == SystemCursor::kHidden;
  if (!cursor_hidden_) {
    cursor_ = ::LoadCursorW(nullptr, CursorResource(cursor));  // 系统共享光标, 无需释放
  }
  if (CursorInClient()) {
    ApplyCursor();
  }
}

bool WinWindow::CursorInClient() const {
  POINT point{};
  if (!::GetCursorPos(&point)) {
    return false;
  }
  HWND hit = ::WindowFromPoint(point);
  if (hit != hwnd_ && !::IsChild(hwnd_, hit)) {
    return false;
  }
  ::ScreenToClient(hwnd_, &point);
  const double dpr = device_pixel_ratio();
  // 缩放边框与控制按钮的光标归系统。
  return HitTestFrame(FrameState(), {point.x / dpr, point.y / dpr}) == WinFrameHit::kClient &&
         point.x >= 0 && point.y >= 0 && point.x < physical_size().width && point.y < physical_size().height;
}

void WinWindow::ApplyCursor() { ::SetCursor(cursor_hidden_ ? nullptr : cursor_); }

// ---------------------------------------------------------------------------
// 窗口框架: 缩放边框、隐藏标题栏、系统控制按钮

WinFrameState WinWindow::FrameState() const {
  WinFrameState s;
  const double dpr = device_pixel_ratio();
  const Size size = physical_size();
  s.client = {size.width / dpr, size.height / dpr};
  s.resize = resize_options_;
  s.maximized = ::IsZoomed(hwnd_) != FALSE;
  s.title_bar_hidden = title_bar_.hidden;
  if (title_bar_.hidden) {
    s.buttons = LayoutCaptionButtons(s.client.width, CaptionHeight(), resize_options_.resizable);
  }
  return s;
}

double WinWindow::CaptionHeight() const {
  if (title_bar_.height > 0) {
    return title_bar_.height;
  }
  // 系统默认: 标准标题栏的高度(标题 + 缩放边框 + 填充)。
  const int physical = ::GetSystemMetricsForDpi(SM_CYCAPTION, dpi_) + ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi_) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
  return physical / device_pixel_ratio();
}

Status WinWindow::SetTitleBar(const TitleBarOptions& options) {
  title_bar_ = options;
  if (options.hidden && caption_buttons_ == nullptr) {
    caption_buttons_ = std::make_unique<WinCaptionButtons>(hwnd_);
  }
  // 隐藏标题栏时外框即内容区: 向内容区延伸 1 像素的 DWM 框架, 保留系统阴影(与 Windows 终端等的做法相同)。
  const MARGINS margins = options.hidden ? MARGINS{0, 0, 1, 0} : MARGINS{0, 0, 0, 0};
  ::DwmExtendFrameIntoClientArea(hwnd_, &margins);
  // 重新计算非客户区(WM_NCCALCSIZE): 窗口外框不变, 内容区包含/不含标题栏后尺寸变化。
  ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  NotifyMetricsChanged();
  NotifyFrameChanged();
  UpdateCaptionButtons();
  NotifyControlsChanged();
  return Status::Ok();
}

std::optional<Rect> WinWindow::window_controls_rect() const {
  if (::IsIconic(hwnd_)) {
    return last_controls_;  // 最小化时客户区为空: 保持最近的值
  }
  return WindowControlsRect(FrameState());
}

void WinWindow::NotifyControlsChanged() {
  if (!created_) return;
  const std::optional<Rect> controls = window_controls_rect();
  if (controls == last_controls_) {
    return;
  }
  last_controls_ = controls;
  delegate_->OnWindowControlsChanged(controls);
}

void WinWindow::UpdateCaptionButtons() {
  if (caption_buttons_ == nullptr) {
    return;
  }
  WinCaptionButtonsAppearance a;
  a.visible = title_bar_.hidden && !::IsIconic(hwnd_);
  if (a.visible) {
    a.layout = FrameState().buttons;
  }
  a.dpr = device_pixel_ratio();
  a.maximized = ::IsZoomed(hwnd_) != FALSE;
  a.active = active_;
  a.dark = dark_;
  a.hover = caption_hover_;
  a.pressed = caption_pressed_;
  caption_buttons_->Update(a);
}

Status WinWindow::SetResizeOptions(const WindowResizeOptions& options) {
  const bool style_changed = options.resizable != resize_options_.resizable;
  resize_options_ = options;
  if (style_changed) {
    ApplyStyle();
  }
  UpdateCaptionButtons();
  return Status::Ok();
}

LRESULT WinWindow::OnNcCalcSize(WPARAM wparam, LPARAM lparam) {
  if (!title_bar_.hidden || wparam == FALSE) {
    return ::DefWindowProcW(hwnd_, WM_NCCALCSIZE, wparam, lparam);
  }
  // 隐藏标题栏: 整个窗口都是内容区(没有非客户区)。最大化时窗口外框超出显示器一圈缩放边框的厚度,
  // 内容区相应内缩, 否则内容的边缘在屏幕之外。
  auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
  if (::IsZoomed(hwnd_)) {
    const int padding = ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
    const int frame_x = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi_) + padding;
    const int frame_y = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi_) + padding;
    RECT& r = params->rgrc[0];
    r.left += frame_x;
    r.right -= frame_x;
    r.top += frame_y;
    r.bottom -= frame_y;
    // 内容区正好铺满显示器时, 系统把窗口当作全屏应用, 自动隐藏的任务栏就呼不出来了: 在任务栏那条边上留 1 像素
    // (Windows 终端、Chromium 的做法)。
    APPBARDATA state{};
    state.cbSize = sizeof(state);
    if ((::SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE) != 0) {
      MONITORINFO monitor{};
      monitor.cbSize = sizeof(monitor);
      if (::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        for (UINT edge : {ABE_TOP, ABE_BOTTOM, ABE_LEFT, ABE_RIGHT}) {
          APPBARDATA bar{};
          bar.cbSize = sizeof(bar);
          bar.uEdge = edge;
          bar.rc = monitor.rcMonitor;
          if (::SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &bar) == 0) continue;
          if (edge == ABE_TOP) r.top += 1;
          if (edge == ABE_BOTTOM) r.bottom -= 1;
          if (edge == ABE_LEFT) r.left += 1;
          if (edge == ABE_RIGHT) r.right -= 1;
        }
      }
    }
  }
  return 0;
}

LRESULT WinWindow::OnNcHitTest(LPARAM lparam) {
  // 系统标题栏与外侧缩放边框(标准框架)照常由系统判断。
  const LRESULT standard = ::DefWindowProcW(hwnd_, WM_NCHITTEST, 0, lparam);
  if (standard != HTCLIENT) {
    return standard;
  }
  POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  ::ScreenToClient(hwnd_, &p);
  const double dpr = device_pixel_ratio();
  return ToHitCode(HitTestFrame(FrameState(), {p.x / dpr, p.y / dpr}));
}

// 宿主绘制的控制按钮: 悬停、按下、点击都在非客户区消息里处理(按钮子窗口不接收鼠标)。不把按钮的按下交给
// DefWindowProc —— 它会画出经典样式的按钮。
bool WinWindow::OnCaptionButtonMessage(UINT message, WPARAM wparam) {
  if (!title_bar_.hidden) {
    return false;
  }
  const WinFrameHit hit = CaptionButtonFromHitCode(wparam);
  switch (message) {
    case WM_NCMOUSEMOVE:
      if (!tracking_nc_leave_) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE | TME_NONCLIENT;
        track.hwndTrack = hwnd_;
        tracking_nc_leave_ = ::TrackMouseEvent(&track) != FALSE;
      }
      caption_hover_ = hit;
      UpdateCaptionButtons();
      // 按钮上的移动不交给 DefWindowProc(它会按经典样式画按钮的悬停);Windows 11 的贴靠布局只依据
      // WM_NCHITTEST 返回的 HTMAXBUTTON。
      return hit != WinFrameHit::kClient;
    case WM_NCMOUSELEAVE:
      tracking_nc_leave_ = false;
      caption_hover_ = WinFrameHit::kClient;
      caption_pressed_ = WinFrameHit::kClient;
      UpdateCaptionButtons();
      return false;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
      if (hit == WinFrameHit::kClient) return false;
      caption_pressed_ = hit;
      UpdateCaptionButtons();
      return true;
    case WM_NCLBUTTONUP: {
      const bool click = hit != WinFrameHit::kClient && caption_pressed_ == hit;
      caption_pressed_ = WinFrameHit::kClient;
      UpdateCaptionButtons();
      if (!click) return hit != WinFrameHit::kClient;
      WPARAM command = SC_CLOSE;
      if (hit == WinFrameHit::kMinimizeButton) command = SC_MINIMIZE;
      if (hit == WinFrameHit::kMaximizeButton) command = ::IsZoomed(hwnd_) ? SC_RESTORE : SC_MAXIMIZE;
      // 经系统命令执行(关闭 → WM_CLOSE → 询问 JS;最小化/最大化带系统动画)。
      ::PostMessageW(hwnd_, WM_SYSCOMMAND, command, 0);
      return true;
    }
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// 位置与系统拖动

// 屏幕框与定位以客户区(momo 视图)为准, 与 macOS 一致;物理像素与全局逻辑点经 WinDisplays 换算。
Rect WinWindow::screen_frame() const {
  RECT client{};
  ::GetClientRect(hwnd_, &client);
  POINT origin{0, 0};
  ::ClientToScreen(hwnd_, &origin);
  return displays_.layout().ToLogical(
      Rect{static_cast<double>(origin.x), static_cast<double>(origin.y),
           static_cast<double>(client.right - client.left), static_cast<double>(client.bottom - client.top)},
      device_pixel_ratio());
}

Status WinWindow::SetPosition(Point top_left) {
  // 客户区左上角 → 窗口外框左上角(差值为标题栏与边框)。
  RECT window{};
  ::GetWindowRect(hwnd_, &window);
  POINT origin{0, 0};
  ::ClientToScreen(hwnd_, &origin);
  const POINT target = displays_.ToPhysical(top_left);
  ::SetWindowPos(hwnd_, nullptr, target.x - (origin.x - window.left), target.y - (origin.y - window.top), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  return Status::Ok();
}

void WinWindow::NotifyFrameChanged() {
  if (!created_) {
    return;
  }
  const Rect frame = screen_frame();
  if (frame == last_frame_) {
    return;
  }
  last_frame_ = frame;
  delegate_->OnWindowFrameChanged(frame);
}

Status WinWindow::StartWindowDrag(int64_t sequence_id) {
  if (!dispatcher().IsMomoSequence(this, sequence_id)) {
    return Status::Ok();  // 序列已结束(往返期间用户已放手)或已不归本窗口
  }
  if (sequence_down_id_ != sequence_id ||
      (sequence_down_message_ != WM_LBUTTONDOWN && sequence_down_message_ != WM_LBUTTONDBLCLK)) {
    return InvalidArgumentError("startDrag: the sequence did not start with a left press in this window");
  }
  if (sequence_down_clicks_ == 2) {
    // 双击标题栏: Windows 的惯例是最大化/还原(不可缩放的窗口什么也不做)。
    if (resize_options_.resizable) {
      ::PostMessageW(hwnd_, WM_SYSCOMMAND, ::IsZoomed(hwnd_) ? SC_RESTORE : SC_MAXIMIZE, 0);
    }
    return Status::Ok();
  }
  // 系统的移动循环(含 Aero Snap、跨屏): 它自己捕获鼠标并吞掉抬起, 返回时拖动已结束。
  capturing_ = false;
  if (::GetCapture() == hwnd_) {
    ::ReleaseCapture();
  }
  POINT cursor{};
  ::GetCursorPos(&cursor);
  const std::weak_ptr<int> alive = alive_;
  ::SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, PointToLParam(cursor));
  if (alive.expired()) {
    return Status::Ok();  // 移动循环期间执行的任务关闭了窗口(有序关闭等): 不再访问本对象
  }
  // 返回时按键已松开则在这里结束序列(否则由派发器的过期判定在下一条消息上结束)。
  const int primary = ::GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
  if ((::GetAsyncKeyState(primary) & 0x8000) == 0) {
    dispatcher().EndSequence(this, CursorScreen(), NowMicros());
  }
  SyncNativeCapture();
  return Status::Ok();
}

Status WinWindow::StartMoveLoop(Point grab_offset) {
  dispatcher().StartMoveLoop(this, grab_offset);  // 不是进行中序列的接收方时什么也不做
  return Status::Ok();
}

void WinWindow::EndMoveLoop() { dispatcher().EndMoveLoop(this); }

StatusOr<bool> WinWindow::CapturePointer(int64_t sequence_id) {
  const bool captured = dispatcher().Capture(this, sequence_id);
  if (captured) {
    SyncNativeCapture();  // 系统把之后的鼠标消息交给本窗口(按下所在的窗口随后关闭也不受影响)
  }
  return captured;
}

void WinWindow::ReleasePointerCapture() {
  dispatcher().ReleaseCapture(this);
  SyncNativeCapture();
}

// ---------------------------------------------------------------------------
// 指针
// 决定接收方的是 PointerDispatcher(embedder/common/pointer_dispatcher.h, 进程内一个, 由 WinMouseInput 持有):
// 这里只把鼠标消息交给 WinMouseInput 翻译后转给它;它经本类(PointerWindow)把事件交给 momo 内容。

PointerDispatcher& WinWindow::dispatcher() const { return mouse_.dispatcher(); }

Point WinWindow::CursorScreen() const {
  POINT p{};
  ::GetCursorPos(&p);
  return displays_.ToLogical(p);
}

void WinWindow::OnMouseMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_MOUSEMOVE) {
    // 持有捕获时不跟踪离开(指针在窗口外时会立刻收到 WM_MOUSELEAVE;序列期间派发器本来也不理会离开)。
    if (!tracking_leave_ && !capturing_) {
      TRACKMOUSEEVENT track{};
      track.cbSize = sizeof(track);
      track.dwFlags = TME_LEAVE;
      track.hwndTrack = hwnd_;
      tracking_leave_ = ::TrackMouseEvent(&track) != FALSE;
    }
    // 系统会发出无位移的 WM_MOUSEMOVE(激活、SetCursor 之后等): 去重。
    const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if (p.x == last_move_.x && p.y == last_move_.y && wparam == last_move_buttons_) {
      return;
    }
    last_move_ = p;
    last_move_buttons_ = wparam;
  } else if (message == WM_MOUSELEAVE) {
    tracking_leave_ = false;
    last_move_ = {INT32_MIN, INT32_MIN};
  }
  WinMouseMessage m;
  m.message = message;
  m.wparam = wparam;
  // 消息产生时的屏幕位置(不经窗口当前位置换算: 拖动中窗口跟随指针移动时两者不同步)。
  const DWORD pos = ::GetMessagePos();
  m.screen = displays_.ToLogical(POINT{GET_X_LPARAM(pos), GET_Y_LPARAM(pos)});
  m.timestamp_us = NowMicros();
  m.extra_info = static_cast<uint32_t>(::GetMessageExtraInfo());
  if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
    UINT lines = 3;
    UINT chars = 3;
    ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    ::SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);
    m.wheel_lines = lines;
    m.wheel_chars = chars;
    m.page_height = physical_size().height / device_pixel_ratio();
  }
  mouse_.Dispatch(m, this);
  SyncNativeCapture();
}

void WinWindow::SyncNativeCapture() {
  if (hwnd_ == nullptr) {
    return;
  }
  const bool want = dispatcher().has_sequence() && dispatcher().PointerOwner(this).has_value();
  if (want) {
    capturing_ = true;
    tracking_leave_ = false;  // 捕获结束后重新跟踪离开
    if (::GetCapture() != hwnd_) {
      ::SetCapture(hwnd_);  // 拖出窗口后仍收到移动与抬起
    }
  } else if (capturing_) {
    capturing_ = false;
    if (::GetCapture() == hwnd_) {
      ::ReleaseCapture();
    }
  }
}

std::optional<int64_t> WinWindow::HitTest(Point screen) const {
  if (!::IsWindowVisible(hwnd_) || ::IsIconic(hwnd_)) {
    return std::nullopt;
  }
  const POINT physical = displays_.ToPhysical(screen);
  // 被别的窗口(本应用的或别的应用的)遮住的地方不属于本窗口。
  HWND hit = ::WindowFromPoint(physical);
  if (hit != hwnd_ && !::IsChild(hwnd_, hit)) {
    return std::nullopt;
  }
  POINT client = physical;
  ::ScreenToClient(hwnd_, &client);
  const Size size = physical_size();
  if (client.x < 0 || client.y < 0 || client.x >= size.width || client.y >= size.height) {
    return std::nullopt;
  }
  // 缩放边框与控制按钮归系统, 不是 momo 内容。
  const double dpr = device_pixel_ratio();
  if (HitTestFrame(FrameState(), {client.x / dpr, client.y / dpr}) != WinFrameHit::kClient) {
    return std::nullopt;
  }
  return 0;  // 没有平台视图: 内容区都是 momo
}

void WinWindow::SendToMomo(platform::PointerDataPacket packet, const PointerInput& input) {
  // 屏幕坐标(逻辑点)→ 客户区物理像素;滚动位移 点 → 物理像素。
  POINT origin{0, 0};
  ::ClientToScreen(hwnd_, &origin);
  const double scale = device_pixel_ratio();
  const WinScreenLayout& layout = displays_.layout();
  for (platform::PointerData& data : packet) {
    const Point physical = layout.ToPhysical(Point{data.screen_x, data.screen_y});
    data.physical_x = physical.x - origin.x;
    data.physical_y = physical.y - origin.y;
    data.scroll_delta_x *= scale;
    data.scroll_delta_y *= scale;
    if (data.phase == platform::PointerPhase::kDown) {
      // 序列的按下(startDrag 据此开始系统拖动或识别双击)。
      const auto* message = static_cast<const WinMouseMessage*>(input.native);
      sequence_down_id_ = data.sequence_id;
      sequence_down_message_ = message != nullptr ? message->message : 0;
      sequence_down_clicks_ = message != nullptr ? message->click_count() : 0;
    }
  }
  delegate_->OnPointerPacket(std::move(packet));
}

// Windows 宿主没有平台视图(capabilities 不含 kCapabilityPlatformViews), 派发器不会选中它们。
void WinWindow::SendToPlatformView(int64_t, const PointerInput&, platform::PointerPhase) {}
void WinWindow::SetHoveredPlatformView(int64_t, const PointerInput&) {}

void WinWindow::MoveTo(Point top_left) { SetPosition(top_left).IgnoreError(); }

// ---------------------------------------------------------------------------
// 键盘

void WinWindow::OnKey(UINT message, WPARAM wparam, LPARAM lparam) {
  const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
  const UINT virtual_key = static_cast<UINT>(wparam);
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
      const std::wstring characters = TranslateKey(virtual_key, scan_code, key_state, layout, &dead_key);
      if (!dead_key && AllPrintable(characters)) {
        data.character = WideToUtf8(characters);
      }
    }
  }
  // 编辑键(退格/方向键/回车等)只走 platform::KeyData, 不再发 kCommand(embedder/common/text_input.h);
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
    // ---- 尺寸 / DPI / 框架 ----
    case WM_NCCALCSIZE:
      return OnNcCalcSize(wparam, lparam);
    case WM_NCHITTEST:
      return OnNcHitTest(lparam);
    case WM_NCACTIVATE:
      if (title_bar_.hidden) {
        // 没有非客户区可画: lParam = -1 让系统不重画框架(只更新激活状态)。
        return ::DefWindowProcW(hwnd_, message, wparam, -1);
      }
      break;
    case WM_NCMOUSEMOVE:
    case WM_NCMOUSELEAVE:
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCLBUTTONUP:
      if (OnCaptionButtonMessage(message, wparam)) {
        return 0;
      }
      break;
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      const SIZE min = WindowSizeForClient(min_size_.width, min_size_.height, dpi_);
      info->ptMinTrackSize.x = min.cx;
      info->ptMinTrackSize.y = min.cy;
      return 0;
    }
    case WM_GETDPISCALEDSIZE: {
      // 跨 DPI 拖动前询问新外框尺寸: 内容区按逻辑尺寸精确换算(默认的线性外框缩放有几像素误差)。
      const UINT new_dpi = static_cast<UINT>(wparam);
      const double dpr = device_pixel_ratio();
      const Size size = physical_size();
      const SIZE window = WindowSizeForClient(size.width / dpr, size.height / dpr, new_dpi);
      auto* out = reinterpret_cast<SIZE*>(lparam);
      *out = window;
      return TRUE;
    }
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);  // 先更新: SetWindowPos 触发的 WM_SIZE 按新 DPR 上报
      displays_.Invalidate();
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      NotifyMetricsChanged();
      UpdateCaptionButtons();
      NotifyControlsChanged();
      return 0;
    }
    case WM_DISPLAYCHANGE:
      displays_.Invalidate();
      NotifyFrameChanged();
      break;
    case WM_ENTERSIZEMOVE:
      in_size_move_ = true;
      // 拖拽缩放的等帧以 1 ms 粒度轮询: 期间把系统定时器精度调到 1 ms(默认 ~15.6 ms 会让每次 WM_SIZE 多等几十毫秒)。
      ::timeBeginPeriod(1);
      return 0;
    case WM_EXITSIZEMOVE:
      if (in_size_move_) {
        ::timeEndPeriod(1);
      }
      in_size_move_ = false;
      NotifyMetricsChanged();
      return 0;
    case WM_SIZE:
      minimized_ = wparam == SIZE_MINIMIZED;
      NotifyStateChanged();  // 最小化 / 最大化 / 还原
      UpdateCaptionButtons();
      if (minimized_) {
        return 0;
      }
      if (in_size_move_) {
        SynchronizeLiveResize();
      } else {
        NotifyMetricsChanged();
      }
      NotifyFrameChanged();
      NotifyControlsChanged();
      return 0;
    case WM_MOVE:
      NotifyFrameChanged();
      return 0;
    case WM_WINDOWPOSCHANGED:
      NotifyStateChanged();  // 显示/隐藏(别处调用 ShowWindow 也会经过这里)
      break;                 // DefWindowProc 据此发 WM_SIZE / WM_MOVE

    // ---- 绘制 ----
    case WM_ERASEBKGND:
      return 1;  // 背景在 WM_PAINT 中一次画完, 避免闪烁
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = ::BeginPaint(hwnd_, &paint);
      if (dc != nullptr) {
        if (d3d_surface_ != nullptr) {
          d3d_surface_->Paint(dc);
        } else if (software_surface_ != nullptr) {
          software_surface_->Paint(dc);
        }
        ::EndPaint(hwnd_, &paint);
      }
      return 0;
    }
    case WinSoftwareSurface::kPresentMessage:
      if (software_surface_ != nullptr) {
        software_surface_->DisplayPending();
      }
      return 0;

    // ---- 窗口与应用的生命周期 ----
    case WM_CLOSE:
      // embedder 总是拦截(返回 false), 询问 JS 后再异步销毁窗口;放行时只隐藏, HWND 由析构销毁。
      if (delegate_->OnCloseRequested()) {
        ::ShowWindow(hwnd_, SW_HIDE);
      }
      return 0;
    case WM_ACTIVATE:
      active_ = LOWORD(wparam) != WA_INACTIVE;
      UpdateCaptionButtons();
      break;
    case WM_ACTIVATEAPP:
      app_.OnActivateApp(wparam != FALSE);
      break;
    case WM_SETFOCUS:
      if (text_input_ != nullptr) text_input_->OnFocusChanged(true);
      if (created_) delegate_->OnFocusChanged(true);
      return 0;
    case WM_KILLFOCUS:
      if (text_input_ != nullptr) text_input_->OnFocusChanged(false);
      if (created_) delegate_->OnFocusChanged(false);
      return 0;
    case WM_SETTINGCHANGE:
      if (lparam != 0 && std::wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
        UpdateDarkMode();
      }
      if (wparam == SPI_SETWORKAREA) {
        displays_.Invalidate();
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
      break;  // 缩放边框、控制按钮等交给系统

    // ---- 指针 ----
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      OnMouseMessage(message, wparam, lparam);
      return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
      OnMouseMessage(message, wparam, lparam);
      return TRUE;  // 文档要求 XBUTTON 消息返回 TRUE
    case WM_CAPTURECHANGED:
      // 捕获被别处拿走(系统菜单、别的应用、改交本应用的另一个窗口): 不在这里结束序列——按键若已松开,
      // 派发器在下一条消息上按过期补报抬起(与 macOS 被嵌套事件循环吞掉抬起时相同)。
      if (reinterpret_cast<HWND>(lparam) != hwnd_) {
        capturing_ = false;
      }
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
