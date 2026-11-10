#pragma once

// PlatformWindow + WindowDelegate(docs/03 §2)。
// 线程约定: PlatformWindow 的所有方法仅在 platform 线程调用(surface() 返回的对象由 raster 线程使用);
// WindowDelegate 回调在 platform 线程触发, 由 embedder 的 WindowController 转投 ui 线程。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/geometry.h"
#include "embedder/common/engine_platform.h"
#include "engine/platform/input_types.h"
#include "engine/platform/platform_view.h"
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
  // 内容区左上角的屏幕位置(逻辑点, 主屏左上原点);为空则居中。
  std::optional<Point> position;
  // 面板: 可成为 key window, 不成为 main window, 聚焦不激活应用(macOS: NSPanel + NonactivatingPanel;
  // 其他平台忽略)。
  bool panel = false;
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
  // 内容区在屏幕上的位置或尺寸变化(逻辑点, 主屏左上原点)。
  virtual void OnWindowFrameChanged(const Rect& screen_frame) { (void)screen_frame; }
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
  // 接管进行中的鼠标按下序列(按下发生在别的窗口): 若仍有按键按住, 在当前指针位置补发一次按下,
  // 之后的拖动与抬起都投递给本窗口(即使按下所在的窗口已关闭), 抬起后自动释放。默认不支持。
  virtual void CapturePointer() {}
  // 隐藏系统标题栏(内容铺满窗口, 保留窗口控制按钮);traffic_center_y: 按钮中心距顶部(逻辑像素, <= 0 默认)。
  virtual void SetTitleBar(bool hidden, double traffic_center_y) {
    (void)hidden;
    (void)traffic_center_y;
  }
  // 可拖动窗口的区域(逻辑像素, 左上原点)。
  virtual void SetDragRegions(const std::vector<Rect>& rects) { (void)rects; }
  // 内容区(momo 视图)在屏幕上的框(逻辑点, 主屏左上原点)。以内容区为准, 屏幕坐标 − 框原点即窗口内
  // 坐标, 与窗口样式(有无标题栏、边框)无关。
  virtual Rect screen_frame() const { return {}; }
  // 移动窗口, 使内容区左上角位于屏幕位置 top_left(逻辑点, 主屏左上原点)。
  virtual void SetPosition(Point top_left) { (void)top_left; }

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
  // 框架对原生指针事件的处置(docs/05 §9 "输入"): 投递原生事件给平台视图 / 指针进入 / 离开;
  // view_id 为 0 的 kEvent: 该按下由 momo 内容消费(键盘焦点交还 momo)。
  virtual void DispatchPlatformViewPointer(int64_t view_id, uint64_t native_event_id,
                                           platform::PlatformViewPointerAction action) {
    (void)view_id;
    (void)native_event_id;
    (void)action;
  }

  platform::ViewportMetrics metrics() const { return {physical_size(), device_pixel_ratio()}; }
};

}  // namespace momo::embedder
