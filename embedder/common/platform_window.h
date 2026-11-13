#pragma once

// PlatformWindow + WindowDelegate(docs/03 §2)。embedder 自己的平台抽象, engine 不可见。
// 窗口的外观与行为(标题、位置、标题栏、拖动、缩放…)都在这一层: JS 经宿主注册的 momo/window 通道
// (window_channel.h)直接调用到这里, 不经过引擎。新增窗口能力 = 在这里加方法、各平台实现、在
// WindowChannel 加一个方法名, 引擎与框架的其他部分不变。
// 线程约定: PlatformWindow 的所有方法仅在 platform 线程调用(surface() 返回的对象由 raster 线程使用);
// WindowDelegate 回调在 platform 线程触发。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/geometry.h"
#include "base/status.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/text_input.h"
#include "embedder/common/window_types.h"
#include "engine/platform/input_types.h"
#include "engine/platform/render_surface.h"
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
  // 辅助窗口: 可接收键盘, 不成为应用的主窗口, 聚焦不激活应用(WindowOptions::non_activating)。
  bool non_activating = false;
};

// 窗口的可选能力(momo/window capabilities)。不支持的能力对应的方法返回 Unimplemented,
// JS 可以先查询再决定界面(如不支持隐藏标题栏时保留系统标题栏)。
enum WindowCapability : uint32_t {
  kCapabilityAlwaysOnTop = 1u << 0,
  kCapabilityPosition = 1u << 1,         // screen_frame / SetPosition
  kCapabilityResizeOptions = 1u << 2,
  kCapabilityHiddenTitleBar = 1u << 3,   // SetTitleBar(hidden) + 窗口控制按钮占用区域
  kCapabilityWindowDrag = 1u << 4,       // StartWindowDrag
  kCapabilityMoveLoop = 1u << 5,         // StartMoveLoop / EndMoveLoop
  kCapabilityPointerCapture = 1u << 6,   // CapturePointer / ReleasePointerCapture
  kCapabilityPlatformViews = 1u << 7,
  kCapabilityNonActivating = 1u << 8,
  kCapabilityVisibility = 1u << 9,       // Hide(Show 总是支持)
  kCapabilityMinMax = 1u << 10,          // Minimize / Maximize / Restore
};

class WindowDelegate {
 public:
  virtual void OnMetricsChanged(const platform::ViewportMetrics& metrics) = 0;
  virtual void OnPointerPacket(platform::PointerDataPacket packet) = 0;
  virtual void OnKeyData(const platform::KeyData& data) = 0;
  // 返回 false 拦截关闭(embedder 总是先拦截, 询问 JS 后再异步关闭)。
  virtual bool OnCloseRequested() = 0;
  virtual void OnFocusChanged(bool focused) = 0;
  // 窗口状态变化(显示/隐藏、最小化、最大化、全屏)。不可见(shown() 为 false)的视图不出帧。
  virtual void OnWindowStateChanged(const WindowStateFlags& state) = 0;
  // IME/文本输入事件(text_input.h), 仅在 TextInputConnection::Show 之后产生。
  virtual void OnTextInput(const TextInputEvent& event) = 0;
  // 内容区在屏幕上的位置或尺寸变化(逻辑点, 主屏左上原点)。
  virtual void OnWindowFrameChanged(const Rect& screen_frame) { (void)screen_frame; }
  // 系统窗口控制按钮在内容区里的占用范围变化(逻辑点, 内容区左上原点;nullopt = 不占用内容区)。
  virtual void OnWindowControlsChanged(const std::optional<Rect>& rect) { (void)rect; }
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
  virtual void SetCursor(SystemCursor cursor) = 0;
  virtual TextInputConnection& text_input() = 0;
  virtual void* native_handle() const = 0;  // HWND / NSView*, 供扩展使用(docs/07)
  // 把窗口带到最前并成为 key window(最小化时先还原)。
  virtual void Activate() { Show(); }
  // 当前状态(显示、最小化、最大化、全屏)。
  virtual WindowStateFlags state() const { return {}; }

  // ---- 可选能力(WindowCapability;默认不支持, 返回 Unimplemented) ----
  virtual uint32_t capabilities() const { return 0; }
  // 隐藏窗口(不销毁;Show 重新显示)。kCapabilityVisibility。
  virtual Status Hide() { return Unsupported("hide"); }
  // 最小化 / 最大化 / 还原(从最小化或最大化恢复)。kCapabilityMinMax。
  virtual Status Minimize() { return Unsupported("minimize"); }
  virtual Status Maximize() { return Unsupported("maximize"); }
  virtual Status Restore() { return Unsupported("restore"); }
  // 浮在普通窗口之上(置顶)。
  virtual Status SetAlwaysOnTop(bool on) {
    (void)on;
    return Unsupported("setAlwaysOnTop");
  }
  // ---- 指针捕获(PointerDispatcher)----
  // 一个按下序列在宿主里只有一个接收方(进程内唯一, 同 Win32 SetCapture): 按下时隐式是按下处的 momo 内容
  // 或平台视图, 固定到最后一个抬起。
  // CapturePointer: 把进行中的 momo 序列 sequence_id 改交本窗口——之后的拖动与抬起都交给本窗口(坐标换算到
  // 本窗口), 即使按下所在的窗口随后关闭。不合成任何事件(框架的逻辑接收方不变, 同一个指针继续)。
  // 返回是否改交成功(序列已结束、id 不匹配或序列归平台视图时为 false, 什么也不做)。
  virtual StatusOr<bool> CapturePointer(int64_t sequence_id) {
    (void)sequence_id;
    return Unsupported("capturePointer");
  }
  // 窗口即将关闭: 如果它是进行中 momo 序列的接收窗口, 以取消结束这个序列(上报 kCancel);
  // 之后到抬起为止的事件不再上报。不是接收窗口时什么也不做(例如序列已改交别的窗口)。
  virtual void ReleasePointerCapture() {}
  // 窗口的缩放行为(WindowResizeOptions)。通道的 setResizable / setResizeBorder 各改其中一部分, 与当前值合并。
  virtual Status SetResizeOptions(const WindowResizeOptions& options) {
    (void)options;
    return Unsupported("setResizable / setResizeBorder");
  }
  virtual WindowResizeOptions resize_options() const { return {}; }
  // 标题栏外观(TitleBarOptions)。系统窗口控制按钮的占用范围经 window_controls_rect() 与
  // WindowDelegate::OnWindowControlsChanged 报告。
  virtual Status SetTitleBar(const TitleBarOptions& options) {
    (void)options;
    return Unsupported("setTitleBar");
  }
  // 系统窗口控制按钮在内容区里的占用范围(逻辑点, 内容区左上原点);标题栏未隐藏时为 nullopt。
  virtual std::optional<Rect> window_controls_rect() const { return std::nullopt; }
  // 以本窗口进行中的 momo 按下序列 sequence_id 开始系统的窗口拖动(哪里可拖由业务层决定, 在按下时调用;
  // 双击按系统设置缩放/最小化)。序列已结束(按键已松开)时什么也不做;id 不匹配时 InvalidArgument。
  // 系统拖动结束后序列随之结束(momo 收到抬起)。
  virtual Status StartWindowDrag(int64_t sequence_id) {
    (void)sequence_id;
    return Unsupported("startDrag");
  }
  // 移动循环: 直到按键松开(或 EndMoveLoop), 宿主在每个鼠标事件上同步移动窗口, 使内容区左上角位于
  // 指针屏幕位置 − grab_offset(逻辑点)。窗口跟随指针由宿主完成, 不经 ui 线程往返。
  virtual Status StartMoveLoop(Point grab_offset) {
    (void)grab_offset;
    return Unsupported("startMoveLoop");
  }
  virtual void EndMoveLoop() {}
  // 内容区(momo 视图)在屏幕上的框(逻辑点, 主屏左上原点)。以内容区为准, 屏幕坐标 − 框原点即窗口内
  // 坐标, 与窗口样式(有无标题栏、边框)无关。不支持时为空矩形。
  virtual Rect screen_frame() const { return {}; }
  // 移动窗口, 使内容区左上角位于屏幕位置 top_left(逻辑点, 主屏左上原点)。
  virtual Status SetPosition(Point top_left) {
    (void)top_left;
    return Unsupported("setPosition");
  }

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

 protected:
  static Status Unsupported(std::string_view what) {
    return UnimplementedError(std::string(what) + " is not supported by this platform window");
  }
};

}  // namespace momo::embedder
