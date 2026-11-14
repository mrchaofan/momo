#pragma once

// WinWindow: HWND 封装 + WndProc(docs/03 §1/§6;对应 macOS 的 MacWindow)。仅被 embedder/windows 的 .cc 包含。
//
//  - DPI: 进程为 Per-Monitor-V2(入口在创建任何窗口前设置)。客户区坐标即物理像素;DPR = GetDpiForWindow / 96。
//    WM_DPICHANGED 采用系统建议矩形;WM_GETDPISCALEDSIZE 精确换算客户区。WindowSpec 的尺寸为逻辑点。
//    屏幕坐标(screen_frame、SetPosition、指针的 screen_x/y)是全局逻辑点, 经 WinDisplays(WinScreenLayout)换算,
//    混合 DPI 的多显示器下也与 macOS 同一约定。
//  - 输入: 鼠标消息经进程内唯一的 WinMouseInput(构造时注入)翻译后交给 PointerDispatcher 决定接收方, 本类作为
//    PointerWindow 执行(交给 momo 的换算为客户区物理像素, 经 delegate 上报)。原生捕获跟随派发器: 本窗口是进行中
//    momo 序列的接收方时持有 SetCapture(序列期间系统把鼠标消息都交给它), 序列结束即释放;capturePointer 把序列
//    改交别的窗口时捕获随之转移。键盘归一化为 KeyData;IME 与 WM_CHAR 交给 WinTextInput。
//  - 窗口框架(win_frame.h): 内侧缩放边框(WindowResizeOptions)经 WM_NCHITTEST 归系统缩放;隐藏标题栏
//    (SetTitleBar)用 WM_NCCALCSIZE 让内容铺满整个窗口, 系统控制按钮由宿主绘制(WinCaptionButtons), 命中返回
//    HTMINBUTTON / HTMAXBUTTON / HTCLOSE(Windows 11 的贴靠布局), 占用区域经 OnWindowControlsChanged 报告。
//    系统拖动(StartWindowDrag)= WM_NCLBUTTONDOWN(HTCAPTION) 进入系统的移动循环(含 Aero Snap);
//    双击按标题栏惯例最大化/还原。移动循环(StartMoveLoop)由派发器在每个拖动上移动窗口(MoveTo)。
//  - 生命周期事件: WM_CLOSE 先问 delegate(embedder 总是拦截, 询问 JS 后再销毁);尺寸/状态/焦点/位置变化经
//    delegate 上报(去重);WM_ACTIVATEAPP 交给 WinApp(应用级前台状态)。
//  - 深色模式: DWMWA_USE_IMMERSIVE_DARK_MODE 与宿主绘制的控制按钮跟随系统"应用模式"。
//  - 光标: WM_SETCURSOR(仅客户区)+ LoadCursorW;kHidden = SetCursor(NULL);缩放边框、控制按钮的光标归系统。
//  - Surface: WinD3DSurface(Direct3D 12 交换链, 失败可回退软件);无 D3D12 设备或环境变量
//    MOMO_SOFTWARE_RENDERING=1 时直接用 GDI 软件 surface(WinSoftwareSurface)。拖拽缩放时在 WM_SIZE 里等一帧
//    新尺寸的内容(SynchronizeLiveResize, 同 macOS)。
//  - 不支持(对 JS 回复 unimplemented): 平台视图、非激活窗口(non_activating 按普通窗口处理)。
// 重入: 拖拽缩放时的等帧(执行平台线程任务)与系统拖动(系统模态循环)期间可能执行任意任务, 包括有序关闭
// (Application::Terminate 同步销毁全部窗口)。这两处在重入返回后先检查 alive_, 窗口已销毁就不再访问本对象。
// 线程约定: platform(主)线程;surface() 由 raster 线程使用(render_surface.h)。

#include <memory>
#include <optional>

#include "base/status.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_window.h"
#include "embedder/common/pointer_dispatcher.h"
#include "embedder/windows/win_frame.h"
#include "embedder/windows/win_mouse_events.h"
#include "embedder/windows/win_util.h"

namespace momo::embedder::windows {

class WinApp;
class WinCaptionButtons;
class WinD3DSurface;
class WinDisplays;
class WinSoftwareSurface;
class WinTextInput;

class WinWindow final : public PlatformWindow, public PointerWindow {
 public:
  // 进程级的输入、显示器与应用对象(WinPlatformIntegration 持有, 比全部窗口活得久)。
  struct Context {
    WinMouseInput& mouse;
    WinDisplays& displays;
    WinApp& app;
  };

  static StatusOr<std::unique_ptr<WinWindow>> Create(const WindowSpec& spec, WindowDelegate* delegate,
                                                     const Context& context);
  ~WinWindow() override;

  // hwnd 是本进程的 momo 窗口时返回它的 WinWindow。
  static WinWindow* FromHwnd(HWND hwnd);
  // z 序最前面的可见 momo 窗口(没有时为空)。
  static HWND FrontmostVisibleHwnd();

  // PlatformWindow
  void Show() override;
  void SetTitle(std::string_view title) override;
  void SetMinSize(Size logical_size) override;
  Size physical_size() const override;
  double device_pixel_ratio() const override;
  platform::RenderSurface& surface() override;
  void SetCursor(SystemCursor cursor) override;
  TextInputConnection& text_input() override;
  void* native_handle() const override { return hwnd_; }
  void Activate() override;
  WindowStateFlags state() const override;
  uint32_t capabilities() const override;
  Status Hide() override;
  Status Minimize() override;
  Status Maximize() override;
  Status Restore() override;
  Status SetAlwaysOnTop(bool on) override;
  StatusOr<bool> CapturePointer(int64_t sequence_id) override;
  void ReleasePointerCapture() override;
  Status SetResizeOptions(const WindowResizeOptions& options) override;
  WindowResizeOptions resize_options() const override { return resize_options_; }
  Status SetTitleBar(const TitleBarOptions& options) override;
  std::optional<Rect> window_controls_rect() const override;
  Status StartWindowDrag(int64_t sequence_id) override;
  Status StartMoveLoop(Point grab_offset) override;
  void EndMoveLoop() override;
  Rect screen_frame() const override;
  Status SetPosition(Point top_left) override;

  // PointerWindow(PointerDispatcher 的执行端, 见 win_window.cc"指针")
  std::optional<int64_t> HitTest(Point screen) const override;
  void SendToMomo(platform::PointerDataPacket packet, const PointerInput& input) override;
  void SendToPlatformView(int64_t view, const PointerInput& input, platform::PointerPhase phase) override;
  void SetHoveredPlatformView(int64_t view, const PointerInput& input) override;
  void MoveTo(Point top_left) override;

  HWND hwnd() const { return hwnd_; }

 private:
  WinWindow(const WindowSpec& spec, WindowDelegate* delegate, const Context& context);

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  Status Initialize(const WindowSpec& spec);
  void CreateSurface(uint32_t background_argb);
  void NotifyMetricsChanged();
  void NotifyFrameChanged();
  void NotifyStateChanged();
  void NotifyControlsChanged();
  void UpdateDarkMode();
  // 逻辑客户区尺寸 → 当前 DPI 下的窗口外框尺寸(标题栏隐藏时外框即客户区)。
  SIZE WindowSizeForClient(double logical_width, double logical_height, UINT dpi) const;
  DWORD style() const;
  DWORD ex_style() const;
  void ApplyStyle();
  // 拖拽缩放中客户区尺寸刚变化: 通知引擎新尺寸, 并等待一帧该尺寸的内容(有上限)。
  void SynchronizeLiveResize();

  // ---- 窗口框架 ----
  WinFrameState FrameState() const;
  double CaptionHeight() const;  // 控制按钮高度(逻辑点)
  LRESULT OnNcCalcSize(WPARAM wparam, LPARAM lparam);
  LRESULT OnNcHitTest(LPARAM lparam);
  // 控制按钮的非客户区鼠标消息。返回 true 表示已处理(不交给 DefWindowProc)。
  bool OnCaptionButtonMessage(UINT message, WPARAM wparam);
  void UpdateCaptionButtons();

  // ---- 指针 ----
  PointerDispatcher& dispatcher() const;
  void OnMouseMessage(UINT message, WPARAM wparam, LPARAM lparam);
  // 原生捕获跟随派发器: 本窗口是进行中序列的接收方时持有捕获, 否则释放。
  void SyncNativeCapture();
  Point CursorScreen() const;  // 当前指针的屏幕位置(逻辑点)
  bool CursorInClient() const;
  void ApplyCursor();

  // ---- 键盘 ----
  void OnKey(UINT message, WPARAM wparam, LPARAM lparam);

  // 存活标记: 可能重入的调用之前取 weak_ptr, 返回后判断本对象是否已被销毁。
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  WindowDelegate* delegate_;
  WinMouseInput& mouse_;
  WinDisplays& displays_;
  WinApp& app_;
  HWND hwnd_ = nullptr;
  UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
  bool created_ = false;  // 创建过程中的消息不上报
  bool minimized_ = false;
  bool in_size_move_ = false;   // 系统的移动/缩放模态循环中
  bool resize_waiting_ = false;
  bool active_ = false;         // 窗口激活(控制按钮外观)
  bool dark_ = false;
  Size min_size_;               // 逻辑点
  WindowResizeOptions resize_options_;
  TitleBarOptions title_bar_;
  Rect last_frame_;
  WindowStateFlags last_state_{.visible = false};
  std::optional<Rect> last_controls_;
  platform::ViewportMetrics last_metrics_;

  std::unique_ptr<platform::RenderSurface> surface_;
  WinD3DSurface* d3d_surface_ = nullptr;      // 指向 surface_(若为 D3D)
  WinSoftwareSurface* software_surface_ = nullptr;  // 当前的软件呈现端(独立的或 D3D 内部回退用的)
  std::unique_ptr<WinTextInput> text_input_;
  std::unique_ptr<WinCaptionButtons> caption_buttons_;
  WinFrameHit caption_hover_ = WinFrameHit::kClient;
  WinFrameHit caption_pressed_ = WinFrameHit::kClient;
  bool tracking_nc_leave_ = false;

  // 指针
  bool tracking_leave_ = false;  // 已 TrackMouseEvent(TME_LEAVE)
  bool capturing_ = false;       // 本窗口为派发器持有 SetCapture
  POINT last_move_{INT32_MIN, INT32_MIN};
  WPARAM last_move_buttons_ = 0;
  // 本窗口 momo 内容收到的最近一个序列的按下(startDrag)。
  int64_t sequence_down_id_ = 0;
  uint32_t sequence_down_message_ = 0;
  int sequence_down_clicks_ = 0;

  // 光标
  HCURSOR cursor_ = nullptr;
  bool cursor_hidden_ = false;
};

}  // namespace momo::embedder::windows
