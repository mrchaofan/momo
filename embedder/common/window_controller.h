#pragma once

// WindowController: PlatformWindow ↔ engine::Shell 视图的粘合层(docs/01 §3)。
//  - 正向: 窗口回调(尺寸/DPI、输入、焦点、关闭请求)→ Shell(由 Shell 转投 ui 线程)。
//  - 反向: 引擎请求(标题、最小尺寸、光标)→ 窗口。
// 引擎附着前收到的 metrics/focus 会被缓存, 附着时补发。
// 多视图: 每个窗口一个 WindowController, 以 view_id 绑定到 Shell(主窗口为视图 0)。
// 线程约定: momo::Object, 归属 platform 线程, 仅在该线程使用。

#include <functional>
#include <memory>

#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_integration.h"
#include "embedder/common/platform_window.h"
#include "engine/platform/vsync_source.h"
#include "engine/shell/shell.h"

namespace momo::embedder {

class WindowController final : public Object,
                               public WindowDelegate,
                               public engine::Shell::Delegate {
 public:
  struct Callbacks {
    // 用户请求关闭窗口(已被拦截)。由 Application 决定是否以及何时真正关闭。
    std::function<void()> on_close_requested;
    // 窗口最小化/还原。
    std::function<void(bool visible)> on_visibility_changed;
  };

  explicit WindowController(Callbacks callbacks,
                            engine::platform::ViewId view_id = engine::platform::kImplicitViewId);
  ~WindowController() override;

  Status CreateWindow(PlatformIntegration& platform, const WindowSpec& spec);
  void DestroyWindow();

  bool has_window() const { return window_ != nullptr; }
  PlatformWindow& window();

  // 绑定/解绑引擎。vsync 为本窗口的 VsyncSource(可为空), 所有权移交 Shell。
  // DetachEngine 返回后 surface 不再被 raster 线程使用;VsyncSource 已 DeleteLater。
  void AttachEngine(engine::Shell& shell, ObjectPtr<platform::VsyncSource> vsync = nullptr);
  void DetachEngine();
  bool has_engine() const { return shell_ != nullptr; }
  engine::platform::ViewId view_id() const { return view_id_; }

  // WindowDelegate
  void OnMetricsChanged(const platform::ViewportMetrics& metrics) override;
  void OnPointerPacket(platform::PointerDataPacket packet) override;
  void OnKeyData(const platform::KeyData& data) override;
  bool OnCloseRequested() override;
  void OnFocusChanged(bool focused) override;
  void OnVisibilityChanged(bool visible) override;
  void OnTextInput(const platform::TextInputEvent& event) override;
  void OnPlatformViewFocused(int64_t view_id) override;
  void OnWindowFrameChanged(const Rect& screen_frame) override;

  // engine::Shell::Delegate
  void OnEngineSetTitle(const std::string& title) override;
  void OnEngineSetMinSize(Size logical_size) override;
  void OnEngineSetCursor(platform::SystemCursor cursor) override;
  void OnEngineSetAlwaysOnTop(bool on) override;
  void OnEngineActivate() override;
  void OnEngineCapturePointer() override;
  void OnEnginePlatformViewPointer(int64_t platform_view_id, uint64_t native_event_id,
                                   platform::PlatformViewPointerAction action) override;
  void OnEngineSetTitleBar(bool hidden, double traffic_center_y) override;
  void OnEngineSetResizeOptions(const platform::WindowResizeOptions& options) override;
  void OnEngineSetDragRegions(const std::vector<Rect>& rects) override;
  void OnEngineSetPosition(Point top_left) override;
  platform::TextInputConnection* GetTextInput() override;
  void* GetNativeWindowHandle() override;
  void OnPlatformViewCreated(int64_t view_id, void* native_view) override;
  void OnPlatformViewDisposed(int64_t view_id) override;
  void OnPlatformViewFocusRequested(int64_t view_id) override;

 private:
  Callbacks callbacks_;
  const engine::platform::ViewId view_id_;
  std::unique_ptr<PlatformWindow> window_;
  engine::Shell* shell_ = nullptr;  // 非拥有;由 Application 拥有

  platform::ViewportMetrics last_metrics_;
  bool has_metrics_ = false;
  bool focused_ = false;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WindowController);
};

}  // namespace momo::embedder
