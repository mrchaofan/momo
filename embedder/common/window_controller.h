#pragma once

// WindowController: PlatformWindow ↔ engine::Shell 视图的粘合层(docs/01 §3)。
//  - 正向: 窗口回调(尺寸/DPI、输入、焦点)→ Shell(由 Shell 转投 ui 线程);窗口级事件(关闭请求、
//    可见性、IME、屏幕框、窗口控制按钮)→ Callbacks(Application 经宿主通道交给 JS)。
//  - 反向: 引擎对视图的请求(平台视图挂载、原生句柄)→ 窗口。窗口外观与行为的请求不经过
//    这里: 宿主的 momo/window 通道直接调用 PlatformWindow。
// 引擎附着前收到的 metrics/focus 会被缓存, 附着时补发。
// 多视图: 每个窗口一个 WindowController, 以 view_id 绑定到 Shell。
// 线程约定: momo::Object, 归属 platform 线程, 仅在该线程使用。

#include <functional>
#include <memory>
#include <optional>

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
    // 窗口获得/失去系统焦点(IME 会话交给获得焦点的窗口)。
    std::function<void(bool focused)> on_focus_changed;
    std::function<void(const TextInputEvent& event)> on_text_input;
    std::function<void(const Rect& screen_frame)> on_frame_changed;
    std::function<void(const std::optional<Rect>& controls)> on_controls_changed;
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
  void OnTextInput(const TextInputEvent& event) override;
  void OnPlatformViewFocused(int64_t view_id) override;
  void OnWindowFrameChanged(const Rect& screen_frame) override;
  void OnWindowControlsChanged(const std::optional<Rect>& rect) override;

  // engine::Shell::Delegate
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
