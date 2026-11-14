#pragma once

// MacWindow: NSWindow + layer-backed 内容视图(MomoContentView)。仅被 .mm 文件包含。
//   - 原生窗口: 普通窗口为 MomoWindow(NSWindow);WindowSpec::non_activating 为 MomoPanel(NSPanel, 非激活面板)。
//     两者只把 -sendEvent: 与平台视图投递转给本对象(SendEvent / DeliverToPlatformView), 行为都在这里。
//   - Surface: 默认 CAMetalLayer(MacMetalSurface, 可回退软件);无 Metal 设备或环境变量
//     MOMO_SOFTWARE_RENDERING=1 时直接用 IOSurface 软件 surface(MacSoftwareSurface)。
//   - 输入: 鼠标/触控板事件经进程内唯一的 MacMouseInput(构造时注入)翻译后交给 PointerDispatcher 决定接收方,
//     本类作为 PointerWindow 执行(交给 momo 的换算为物理像素、左上原点, 经 delegate 上报;交给平台视图的经
//     输入路由投递);键盘事件经 mac_key_mapping 归一化为 KeyData 经 delegate 上报。
//   - IME: 视图实现 NSTextInputClient, 委托给 MacTextInput(text_input())。
//   - 光标: SetCursor 用 NSCursor(kHidden 只在鼠标位于视图内时隐藏, 离开即恢复)。
//   - 平台视图(docs/05 §9): MomoPlatformViews 宿主挂在内容视图上;几何由 Metal surface 的合成帧驱动。
//     点击 momo 区域时内容视图取回第一响应者;平台视图获得焦点时经 delegate 通知引擎。
// 线程约定: platform(主)线程;surface() 由 raster 线程使用(render_surface.h)。

#include <memory>
#include <optional>

#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_window.h"
#include "embedder/common/pointer_dispatcher.h"

@class NSEvent;
@class NSHashTable<ObjectType>;
@class NSTrackingArea;
@class NSWindow;
@class MomoContentView;
@class MomoWindowObserver;
@class MomoPlatformViews;
@class MomoPlatformViewInputRouter;

// 原生窗口把事件交给 AppKit 的父类实现(-[NSWindow sendEvent:])。
typedef void (^MomoSuperSendEvent)(NSEvent* event);

namespace momo::embedder::macos {

class MacMetalSurface;
class MacMouseInput;
class MacTextInput;

class MacWindow final : public PlatformWindow, public PointerWindow {
 public:
  // mouse 必须比窗口活得久(MacPlatformIntegration 持有)。
  MacWindow(const WindowSpec& spec, WindowDelegate* delegate, MacMouseInput& mouse);
  ~MacWindow() override;

  // PlatformWindow
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
  WindowStateFlags state() const override;
  uint32_t capabilities() const override;
  Status Hide() override;
  Status Minimize() override;
  Status Maximize() override;
  Status Restore() override;
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
  void AddPlatformView(int64_t view_id, void* native_view) override;
  void RemovePlatformView(int64_t view_id) override;
  void FocusPlatformView(int64_t view_id) override;

  // PointerWindow(PointerDispatcher 的执行端, 见 mac_window.mm"指针")
  std::optional<int64_t> HitTest(Point screen) const override;
  void SendToMomo(platform::PointerDataPacket packet, const PointerInput& input) override;
  void SendToPlatformView(int64_t view, const PointerInput& input, platform::PointerPhase phase) override;
  void SetHoveredPlatformView(int64_t view, const PointerInput& input) override;
  void MoveTo(Point top_left) override;

  // ---- 供原生窗口(MomoWindow / MomoPanel)调用 ----
  // 鼠标与其他位置型输入的入口(-sendEvent:), 其余事件交给 super。
  void SendEvent(NSEvent* event, MomoSuperSendEvent super_send);
  // MomoMouseRouting: 经 AppKit 的正常投递把事件交给平台视图 view_id(投递期间命中测试只落在它的子树里)。
  void DeliverToPlatformView(NSEvent* event, int64_t view_id, MomoSuperSendEvent super_send);
  int64_t routed_platform_view() const { return routed_platform_view_; }

  // ---- 供内容视图与窗口观察者调用 ----
  WindowDelegate* delegate() const { return delegate_; }
  MacTextInput& mac_text_input() { return *text_input_; }
  MacMetalSurface* metal_surface() const { return metal_surface_; }  // 软件 surface 时为空
  MomoPlatformViews* platform_views() const { return platform_views_; }
  // 指针此刻归本窗口的谁(PointerDispatcher::PointerOwner)。
  std::optional<int64_t> PointerOwner() const;
  void NotifyMetricsChanged();
  void NotifyFrameChanged();
  // 窗口状态(显示、最小化、缩放、全屏)可能变化: 有变化时通知 delegate。
  void NotifyStateChanged();
  // 系统窗口控制按钮(红绿灯)的占用范围可能变化(标题栏样式、尺寸、全屏): 有变化时通知 delegate。
  void NotifyControlsChanged();
  // 隐藏标题栏时把红绿灯垂直居中到应用标题栏高度内(尺寸变化、退出全屏后重新布局)。
  void LayoutTrafficLights();
  // 拖拽缩放中视图尺寸刚变化(platform 线程): 通知引擎新尺寸, 并等待一帧该尺寸的内容与新窗口尺寸同一事务提交
  // (同 Flutter FlutterResizeSynchronizer)。等待有上限(kLiveResizeFrameWait), 超时则暂时拉伸旧内容。
  // 这是宿主的实现细节, 不对应用暴露。
  void SynchronizeLiveResize();

 private:
  PointerDispatcher& dispatcher() const;
  // 不经 AppKit 处理的按下: 自行完成点击激活(面板只成为 key, 不激活应用)。
  void ActivateForMomoClick();
  // 窗口装饰(内容视图之外)的跟踪区记账: 由有序输入流推导进入/离开、转发移动。返回装饰是否改变了光标。
  bool ForwardToWindowChrome(NSEvent* event);

  WindowDelegate* delegate_;
  MacMouseInput& mouse_;
  NSWindow* window_;
  MomoContentView* view_;
  MomoWindowObserver* observer_;
  MomoPlatformViews* platform_views_;
  MomoPlatformViewInputRouter* input_router_;  // 平台视图的输入投递(谁接收由 PointerDispatcher 决定)
  NSHashTable<NSTrackingArea*>* chrome_inside_;  // 窗口装饰已投递进入、尚未投递离开的跟踪区
  NSEvent* sequence_down_ = nullptr;  // 本窗口 momo 内容收到的最近一个序列的按下(startDrag)
  int64_t sequence_down_id_ = 0;
  bool non_activating_ = false;  // NSPanel(非激活面板): 显示/激活时不激活应用
  bool system_tracking_ = false;  // 当前按下序列交给了 AppKit(红绿灯等窗口控件), 直到抬起
  int64_t routed_platform_view_ = 0;  // 正在经 AppKit 投递给的平台视图(0 = 无;kNoPlatformView = 不许落到平台视图)
  Rect last_frame_;
  WindowStateFlags last_state_;
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
