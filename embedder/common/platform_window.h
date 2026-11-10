#pragma once

// PlatformWindow + WindowDelegate(docs/03 §2)。
// 线程约定: PlatformWindow 的所有方法仅在 platform 线程调用(surface() 返回的对象由 raster 线程使用);
// WindowDelegate 回调在 platform 线程触发, 由 embedder 的 WindowController 转投 ui 线程。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/geometry.h"
#include "embedder/common/engine_platform.h"
#include "engine/platform/input_types.h"
#include "engine/platform/render_surface.h"
#include "engine/platform/system_cursor.h"
#include "engine/platform/text_input_connection.h"
#include "engine/platform/viewport_metrics.h"

namespace momo::embedder {

struct WindowSpec {
  std::string title = "Momo";
  Size size{1024, 768};      // 逻辑像素
  Size min_size{200, 150};   // 逻辑像素
  bool resizable = true;
  uint32_t background_argb = 0xFFFFFFFF;  // 首帧前的闪屏底色
};

class WindowDelegate {
 public:
  virtual void OnMetricsChanged(const platform::ViewportMetrics& metrics) = 0;
  virtual void OnPointerPacket(platform::PointerDataPacket packet) = 0;
  virtual void OnKeyData(const platform::KeyData& data) = 0;
  // 返回 false 拦截关闭(embedder 总是先拦截, 询问 JS 后再异步关闭)。
  virtual bool OnCloseRequested() = 0;
  virtual void OnFocusChanged(bool focused) = 0;
  // 最小化/还原(Mac miniaturize, Win SIZE_MINIMIZED), 用于生命周期 Suspended。
  virtual void OnVisibilityChanged(bool visible) = 0;
  // IME/文本输入事件(text_input_connection.h), 仅在 platform::TextInputConnection::Show 之后产生。
  virtual void OnTextInput(const platform::TextInputEvent& event) = 0;
  // 平台视图获得键盘焦点(用户点击等, docs/05 §9)。
  virtual void OnPlatformViewFocused(int64_t view_id) { (void)view_id; }

 protected:
  ~WindowDelegate() = default;
};

class PlatformWindow {
 public:
  virtual ~PlatformWindow() = default;
  virtual void Show() = 0;
  virtual void SetTitle(std::string_view title) = 0;
  virtual void SetMinSize(Size logical_size) = 0;
  virtual Size physical_size() const = 0;
  virtual double device_pixel_ratio() const = 0;
  virtual platform::RenderSurface& surface() = 0;
  virtual void SetCursor(platform::SystemCursor cursor) = 0;
  virtual platform::TextInputConnection& text_input() = 0;
  virtual void* native_handle() const = 0;  // HWND / NSView*, 供扩展使用(docs/07)

  // 浮在普通窗口之上(置顶)。默认不支持。
  virtual void SetAlwaysOnTop(bool on) { (void)on; }
  // 带到最前并成为 key window。
  virtual void Activate() { Show(); }
  // 隐藏系统标题栏(内容铺满窗口, 保留窗口控制按钮);traffic_center_y: 按钮中心距顶部(逻辑像素, <= 0 默认)。
  virtual void SetTitleBar(bool hidden, double traffic_center_y) {
    (void)hidden;
    (void)traffic_center_y;
  }
  // 可拖动窗口的区域(逻辑像素, 左上原点)。
  virtual void SetDragRegions(const std::vector<Rect>& rects) { (void)rects; }

  // ---- 平台视图(docs/05 §9;默认不支持)----
  // 挂入/移出窗口的视图层级。几何与可见性由 surface 的合成帧(RenderSurface::AcquireCompositionFrame)
  // 驱动, 挂入时初始隐藏。native_view 由扩展所有, 窗口在挂入期间额外持有引用。
  virtual void AddPlatformView(int64_t view_id, void* native_view) {
    (void)view_id;
    (void)native_view;
  }
  virtual void RemovePlatformView(int64_t view_id) { (void)view_id; }
  // 把键盘焦点交给平台视图。
  virtual void FocusPlatformView(int64_t view_id) { (void)view_id; }

  platform::ViewportMetrics metrics() const { return {physical_size(), device_pixel_ratio()}; }
};

}  // namespace momo::embedder
