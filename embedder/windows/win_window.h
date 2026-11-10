#pragma once

// WinWindow: HWND 封装 + WndProc(docs/03 §1/§6)。仅被 embedder/windows 的 .cc 包含。
// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。
//
//  - DPI: 进程须为 Per-Monitor-V2(embedder 在创建任何窗口前设置)。客户区坐标即物理像素;
//    DPR = GetDpiForWindow / 96。WM_DPICHANGED 采用系统建议矩形;WM_GETDPISCALEDSIZE 精确换算客户区。
//    WindowSpec 的尺寸/最小尺寸为逻辑像素, 用 AdjustWindowRectExForDpi 换算为窗口外框。
//  - metrics: WM_SIZE(非最小化)/ WM_DPICHANGED 后上报 {客户区物理尺寸, DPR}(去重, 与 mac 一致);
//    创建过程中的消息不上报(初始值由 metrics() 取得)。
//  - 生命周期事件: WM_CLOSE 先问 delegate(返回 true 才隐藏窗口, HWND 由析构销毁);
//    SIZE_MINIMIZED / 还原 → OnVisibilityChanged;WM_SETFOCUS / WM_KILLFOCUS → OnFocusChanged。
//  - 深色标题栏: DWMWA_USE_IMMERSIVE_DARK_MODE 跟随系统"应用模式"(注册表 AppsUseLightTheme),
//    WM_SETTINGCHANGE("ImmersiveColorSet")时更新。
//  - 输入: 鼠标/滚轮/键盘归一化为 PointerData/KeyData(物理像素、左上原点、与 mac 同一套约定), 经 delegate
//    上报;IME 与 WM_CHAR 交给 WinTextInput。
//  - 光标: WM_SETCURSOR(仅客户区)+ LoadCursorW;kHidden = SetCursor(NULL), 离开客户区即恢复系统光标。
//  - Surface: WinSoftwareSurface(GPU/ANGLE 未实现)。
// 线程约定: platform(主)线程;surface() 由 raster 线程使用(render_surface.h)。

#include <memory>

#include "base/status.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_window.h"
#include "embedder/windows/win_util.h"

namespace momo::embedder::windows {

class WinSoftwareSurface;
class WinTextInput;

class WinWindow final : public PlatformWindow {
 public:
  static StatusOr<std::unique_ptr<WinWindow>> Create(const WindowSpec& spec,
                                                     WindowDelegate* delegate);
  ~WinWindow() override;

  void Show() override;
  void SetTitle(std::string_view title) override;
  void SetMinSize(Size logical_size) override;
  Size physical_size() const override;
  double device_pixel_ratio() const override;
  platform::RenderSurface& surface() override;
  void SetCursor(platform::SystemCursor cursor) override;
  void CapturePointer() override;
  Rect screen_frame() const override;
  void SetPosition(Point top_left) override;
  platform::TextInputConnection& text_input() override;
  void* native_handle() const override { return hwnd_; }

  HWND hwnd() const { return hwnd_; }

 private:
  WinWindow(const WindowSpec& spec, WindowDelegate* delegate);

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  Status Initialize(const WindowSpec& spec);
  void NotifyMetricsChanged();
  void NotifyFrameChanged();
  void UpdateDarkMode();
  // 逻辑客户区尺寸 → 当前 DPI 下的窗口外框尺寸。
  SIZE WindowSizeForClient(double logical_width, double logical_height, UINT dpi) const;
  DWORD style() const;
  DWORD ex_style() const;

  // ---- 指针 ----
  void OnMouseButton(UINT message, WPARAM wparam, LPARAM lparam);
  void OnMouseMove(LPARAM lparam);
  void OnMouseLeave();
  void OnMouseWheel(UINT message, WPARAM wparam, LPARAM lparam);
  void OnCaptureLost();
  platform::PointerData MakePointer(platform::PointerPhase phase, double x, double y) const;
  void DispatchPointer(platform::PointerData data);
  bool CursorInClient() const;
  void ApplyCursor();

  // ---- 键盘 ----
  void OnKey(UINT message, WPARAM wparam, LPARAM lparam);

  WindowDelegate* delegate_;
  HWND hwnd_ = nullptr;
  UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
  bool resizable_ = true;
  bool created_ = false;     // 创建过程中的消息不上报
  Rect last_frame_;          // 最近上报的内容区屏幕框
  bool minimized_ = false;
  Size min_size_;            // 逻辑像素
  std::unique_ptr<WinSoftwareSurface> surface_;
  std::unique_ptr<WinTextInput> text_input_;
  platform::ViewportMetrics last_metrics_;

  // 指针状态
  uint32_t buttons_ = 0;          // 当前按下的按钮位图
  bool pointer_added_ = false;    // 已发出 kAdd(尚未 kRemove)
  bool tracking_leave_ = false;   // 已 TrackMouseEvent(TME_LEAVE)
  double last_x_ = 0;
  double last_y_ = 0;
  platform::PointerDeviceKind last_kind_ = platform::PointerDeviceKind::kMouse;

  // 光标
  HCURSOR cursor_ = nullptr;
  bool cursor_hidden_ = false;
};

}  // namespace momo::embedder::windows
