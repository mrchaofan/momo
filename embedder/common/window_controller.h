#pragma once

// WindowController: PlatformWindow ↔ engine::Shell 视图的粘合层(ARCHITECTURE.md §3.4)。
//  - 正向: 窗口回调(尺寸/DPI、输入、焦点、可见性)→ Shell(由 Shell 转投 ui 线程);纯通知类的窗口事件
//    (状态、焦点、屏幕框、窗口控制按钮、首帧)与输入法事件 → 直接交给注入的通道(JS);关闭请求需要
//    WindowManager 做决定 → Events::close_requested。
//  - 反向: 引擎对视图的请求(平台视图挂载、原生句柄)→ 窗口。窗口外观与行为的请求不经过
//    这里: 宿主的 momo/window 通道直接调用 PlatformWindow。
// 去重: 平台窗口上报的是原始值(可以重复), 尺寸、状态、屏幕框、控制按钮的"与上次相同则不转发"只在这里做。
// 生命周期: 创建即绑定引擎;Detach 之后到销毁之前(关闭的窗口延后到安全点销毁)窗口的回调都是空操作。
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
#include "embedder/common/text_input_channel.h"
#include "embedder/common/window_channel.h"
#include "engine/shell/shell.h"

namespace momo::embedder {

class WindowController final : public Object,
                               public WindowDelegate,
                               public engine::Shell::Delegate {
 public:
  // 窗口事件的去向。三者都必须在 Detach 之前保持有效。
  struct Bindings {
    engine::Shell& shell;
    WindowChannel& window_events;
    TextInputChannel& text_input;
    // 用户请求关闭窗口(原生关闭已被拦截): 由 WindowManager 决定是否以及何时真正关闭。
    std::function<void()> close_requested;
  };

  // 创建平台窗口(不显示)及其控制器, 并绑定为引擎的视图 view_id。
  static StatusOr<std::unique_ptr<WindowController>> Create(PlatformIntegration& platform,
                                                           const WindowOptions& options,
                                                           engine::platform::ViewId view_id,
                                                           Bindings bindings);
  ~WindowController() override;  // 销毁窗口(须先 Detach)

  PlatformWindow& window() { return *window_; }

  // 解绑引擎: 返回后 surface 不再被 raster 线程使用, 也不再向通道发通知。幂等。
  void Detach();
  engine::platform::ViewId view_id() const { return view_id_; }
  const WindowStateFlags& state() const { return state_; }
  // 窗口是否持有系统焦点。
  bool focused() const { return focused_; }

  // WindowDelegate
  void OnMetricsChanged(const platform::ViewportMetrics& metrics) override;
  void OnPointerPacket(platform::PointerDataPacket packet) override;
  void OnKeyData(const platform::KeyData& data) override;
  void OnCloseRequested() override;
  void OnFocusChanged(bool focused) override;
  void OnWindowStateChanged(const WindowStateFlags& state) override;
  void OnTextInput(const TextInputEvent& event) override;
  void OnPlatformViewFocused(int64_t view_id) override;
  void OnWindowFrameChanged(const Rect& screen_frame) override;
  void OnWindowControlsChanged(const std::optional<Rect>& rect) override;

  // engine::Shell::Delegate
  void OnPlatformViewCreated(int64_t view_id, void* native_view) override;
  void OnPlatformViewDisposed(int64_t view_id) override;
  void OnPlatformViewFocusRequested(int64_t view_id) override;
  void OnPlatformViewBlurRequested(int64_t view_id) override;
  void OnFirstFramePresented() override;

 private:
  WindowController(engine::platform::ViewId view_id, Bindings bindings);

  void Attach();
  bool ShouldRender() const;
  void UpdateRendering();

  const engine::platform::ViewId view_id_;
  const Bindings bindings_;
  // 只在 Create 期间为空: 平台窗口在自己的构造过程中就可能回调(定位、布局), 那时的回调只更新下面的缓存。
  std::unique_ptr<PlatformWindow> window_;
  // 窗口已建好且尚未 Detach: 只有这期间才把窗口的回调交给引擎与通道。
  bool attached_ = false;

  // 最近一次上报的值(去重)。屏幕框在第一次上报前为空: 第一次总是转发。
  platform::ViewportMetrics metrics_;
  WindowStateFlags state_;
  std::optional<Rect> frame_;
  std::optional<Rect> controls_;
  bool focused_ = false;
  bool first_frame_ = false;  // 绑定引擎后已呈现过一帧
  bool rendering_ = true;     // 已告知引擎的可见性

  MOMO_DISALLOW_COPY_AND_ASSIGN(WindowController);
};

}  // namespace momo::embedder
