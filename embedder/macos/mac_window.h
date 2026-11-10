#pragma once

// MacWindow: NSWindow + layer-backed 内容视图(MomoContentView)。仅被 .mm 文件包含。
//   - Surface: 默认 CAMetalLayer(MacMetalSurface, 可回退软件);无 Metal 设备或环境变量
//     MOMO_SOFTWARE_RENDERING=1 时直接用 IOSurface 软件 surface(MacSoftwareSurface)。
//   - 输入: 视图把鼠标/触控板/键盘事件归一化为 PointerData/KeyData(物理像素、左上原点), 经 delegate 上报。
//   - IME: 视图实现 NSTextInputClient, 委托给 MacTextInput(text_input())。
//   - 光标: SetCursor 用 NSCursor(kHidden 只在鼠标位于视图内时隐藏, 离开即恢复)。
//   - 平台视图(docs/05 §9): MomoPlatformViews 宿主挂在内容视图上;几何由 Metal surface 的合成帧驱动。
//     点击 momo 区域时内容视图取回第一响应者;平台视图获得焦点时经 delegate 通知引擎。
// 线程约定: platform(主)线程;surface() 由 raster 线程使用(render_surface.h)。

#include <memory>
#include <vector>

#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_window.h"

#ifdef __OBJC__
@class NSWindow;
@class MomoContentView;
@class MomoWindowObserver;
@class MomoPlatformViews;
#else
typedef struct objc_object NSWindow;
typedef struct objc_object MomoContentView;
typedef struct objc_object MomoWindowObserver;
typedef struct objc_object MomoPlatformViews;
#endif

namespace momo::embedder::macos {

class MacMetalSurface;
class MacTextInput;

class MacWindow final : public PlatformWindow {
 public:
  MacWindow(const WindowSpec& spec, WindowDelegate* delegate);
  ~MacWindow() override;

  void Show() override;
  void SetTitle(std::string_view title) override;
  void SetMinSize(Size logical_size) override;
  Size physical_size() const override;
  double device_pixel_ratio() const override;
  platform::RenderSurface& surface() override;
  void SetCursor(platform::SystemCursor cursor) override;
  platform::TextInputConnection& text_input() override;
  void* native_handle() const override;
  void SetAlwaysOnTop(bool on) override;
  void Activate() override;
  void CapturePointer() override;
  void SetTitleBar(bool hidden, double traffic_center_y) override;
  void SetDragRegions(const std::vector<Rect>& rects) override;
  Rect screen_frame() const override;
  void SetPosition(Point top_left) override;
  void NotifyFrameChanged();
  // 隐藏标题栏时把红绿灯垂直居中到 traffic_center_y_(尺寸变化、退出全屏后重新布局)。
  void LayoutTrafficLights();
  void AddPlatformView(int64_t view_id, void* native_view) override;
  void RemovePlatformView(int64_t view_id) override;
  void FocusPlatformView(int64_t view_id) override;

  // 供 ObjC 视图/观察者回调。
  WindowDelegate* delegate() const { return delegate_; }
  MacTextInput& mac_text_input() { return *text_input_; }
  MacMetalSurface* metal_surface() const { return metal_surface_; }  // 软件 surface 时为空
  NSWindow* ns_window() const { return window_; }
  MomoPlatformViews* platform_views() const { return platform_views_; }
  void NotifyMetricsChanged();

 private:
  WindowDelegate* delegate_;
  NSWindow* window_;
  MomoContentView* view_;
  MomoWindowObserver* observer_;
  MomoPlatformViews* platform_views_;
  bool titlebar_hidden_ = false;
  bool panel_ = false;  // NSPanel(非激活面板): 显示/激活时不激活应用
  Rect last_frame_;
  double traffic_center_y_ = 0;
  std::unique_ptr<platform::RenderSurface> surface_;
  MacMetalSurface* metal_surface_ = nullptr;  // 指向 surface_(若为 Metal)
  std::unique_ptr<MacTextInput> text_input_;
  platform::ViewportMetrics last_metrics_;
};

}  // namespace momo::embedder::macos
