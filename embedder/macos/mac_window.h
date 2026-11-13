#pragma once

// MacWindow: NSWindow + layer-backed 内容视图(MomoContentView)。仅被 .mm 文件包含。
//   - Surface: 默认 CAMetalLayer(MacMetalSurface, 可回退软件);无 Metal 设备或环境变量
//     MOMO_SOFTWARE_RENDERING=1 时直接用 IOSurface 软件 surface(MacSoftwareSurface)。
//   - 输入: 鼠标/触控板事件翻译后交给进程内唯一的 PointerDispatcher 决定接收方, 本类作为 PointerWindow
//     执行(交给 momo 的换算为物理像素、左上原点, 经 delegate 上报;交给平台视图的经输入路由投递);
//     键盘事件归一化为 KeyData 经 delegate 上报。
//   - IME: 视图实现 NSTextInputClient, 委托给 MacTextInput(text_input())。
//   - 光标: SetCursor 用 NSCursor(kHidden 只在鼠标位于视图内时隐藏, 离开即恢复)。
//   - 平台视图(docs/05 §9): MomoPlatformViews 宿主挂在内容视图上;几何由 Metal surface 的合成帧驱动。
//     点击 momo 区域时内容视图取回第一响应者;平台视图获得焦点时经 delegate 通知引擎。
// 线程约定: platform(主)线程;surface() 由 raster 线程使用(render_surface.h)。

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "base/time.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_window.h"
#include "embedder/common/pointer_dispatcher.h"

#ifdef __OBJC__
@class NSWindow;
@class MomoContentView;
@class MomoWindowObserver;
@class MomoPlatformViews;
@class MomoPlatformViewInputRouter;
@class NSEvent;
#else
typedef struct objc_object NSWindow;
typedef struct objc_object MomoContentView;
typedef struct objc_object MomoWindowObserver;
typedef struct objc_object MomoPlatformViews;
typedef struct objc_object MomoPlatformViewInputRouter;
typedef struct objc_object NSEvent;
#endif

namespace momo::embedder::macos {

class MacMetalSurface;
class MacTextInput;

class MacWindow final : public PlatformWindow, public PointerWindow {
 public:
  MacWindow(const WindowSpec& spec, WindowDelegate* delegate);
  ~MacWindow() override;

  void Show() override;
  void SetTitle(std::string_view title) override;
  void SetMinSize(Size logical_size) override;
  Size physical_size() const override;
  double device_pixel_ratio() const override;
  platform::RenderSurface& surface() override;
  void SetCursor(SystemCursor cursor) override;
  TextInputConnection& text_input() override;
  void* native_handle() const override;
  void Activate() override;
  uint32_t capabilities() const override;
  Status SetAlwaysOnTop(bool on) override;
  StatusOr<bool> CapturePointer(int64_t sequence_id) override;
  void ReleasePointerCapture() override;
  Status SetTitleBar(const TitleBarOptions& options) override;
  std::optional<Rect> window_controls_rect() const override;
  Status SetResizeOptions(const WindowResizeOptions& options) override;
  WindowResizeOptions resize_options() const override { return resize_options_; }
  Status StartWindowDrag(int64_t sequence_id) override;
  Status StartMoveLoop(Point grab_offset) override;
  void EndMoveLoop() override;
  Rect screen_frame() const override;
  Status SetPosition(Point top_left) override;
  void NotifyFrameChanged();
  // 系统窗口控制按钮(红绿灯)的占用范围可能变化(标题栏样式、尺寸、全屏): 有变化时通知 delegate。
  void NotifyControlsChanged();
  // 隐藏标题栏时把红绿灯垂直居中到应用标题栏高度内(尺寸变化、退出全屏后重新布局)。
  void LayoutTrafficLights();
  void AddPlatformView(int64_t view_id, void* native_view) override;
  void RemovePlatformView(int64_t view_id) override;
  void FocusPlatformView(int64_t view_id) override;

  // PointerWindow(PointerDispatcher 的执行端, 见 mac_window.mm"指针")
  std::optional<int64_t> HitTest(Point screen) const override;
  void SendToMomo(platform::PointerDataPacket packet, const PointerInput& input) override;
  void SendToPlatformView(int64_t view, const PointerInput& input, platform::PointerPhase phase) override;
  void SetHoveredPlatformView(int64_t view, const PointerInput& input) override;
  void MoveTo(Point top_left) override;


  // 供 ObjC 视图/观察者回调。
  WindowDelegate* delegate() const { return delegate_; }
  MacTextInput& mac_text_input() { return *text_input_; }
  MacMetalSurface* metal_surface() const { return metal_surface_; }  // 软件 surface 时为空
  NSWindow* ns_window() const { return window_; }
  MomoPlatformViews* platform_views() const { return platform_views_; }
  MomoPlatformViewInputRouter* input_router() const { return input_router_; }
  void NotifyMetricsChanged();
  // 拖拽缩放中视图尺寸刚变化(platform 线程): 通知引擎新尺寸, 并等待一帧该尺寸的内容与新窗口尺寸同一事务提交
  // (同 Flutter FlutterResizeSynchronizer)。等待有上限(kLiveResizeFrameWait), 超时则暂时拉伸旧内容。
  // 这是宿主的实现细节, 不对应用暴露。
  void SynchronizeLiveResize();

 private:
  WindowDelegate* delegate_;
  NSWindow* window_;
  MomoContentView* view_;
  MomoWindowObserver* observer_;
  MomoPlatformViews* platform_views_;
  MomoPlatformViewInputRouter* input_router_;  // 平台视图的输入投递(谁接收由 PointerDispatcher 决定)
  NSEvent* sequence_down_ = nullptr;  // 本窗口 momo 内容收到的最近一个序列的按下(startDrag)
  int64_t sequence_down_id_ = 0;
  bool non_activating_ = false;  // NSPanel(非激活面板): 显示/激活时不激活应用
  Rect last_frame_;
  TitleBarOptions title_bar_;
  std::optional<Rect> last_controls_;
  std::unique_ptr<platform::RenderSurface> surface_;
  MacMetalSurface* metal_surface_ = nullptr;  // 指向 surface_(若为 Metal)
  std::unique_ptr<MacTextInput> text_input_;
  platform::ViewportMetrics last_metrics_;
  bool resize_waiting_ = false;
  // 缩放行为(momo/window setResizable / setResizeBorder;resizable 由 WindowSpec 初始化)。
  WindowResizeOptions resize_options_;
};

}  // namespace momo::embedder::macos

#ifdef __OBJC__
// 进程级本地事件监视器的判定(测试也直接调用): 按下序列进行中、事件不属于任何 momo 窗口时交给派发器并返回
// true;否则返回 false(照常派发, 由所属窗口交给派发器)。见 mac_window.mm。
bool MomoRoutePointerSequenceEvent(NSEvent* event);
// 是否有进行中的按下序列(进程内, 任一窗口)。
bool MomoPointerSequenceActive();
#endif
