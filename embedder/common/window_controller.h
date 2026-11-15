#pragma once

// WindowController: PlatformWindow ↔ engine::Shell 视图的粘合层(docs/01 §3)。
//  - 正向: 窗口回调(尺寸/DPI、输入、焦点、可见性)→ Shell(由 Shell 转投 ui 线程);纯通知类的窗口事件
//    (状态、焦点、屏幕框、窗口控制按钮、首帧)→ 直接交给注入的 WindowChannel(JS);需要 WindowManager
//    做决定的事件(关闭请求、焦点归属、IME 路由)→ Client。
//  - 反向: 引擎对视图的请求(平台视图挂载、原生句柄)→ 窗口。窗口外观与行为的请求不经过
//    这里: 宿主的 momo/window 通道直接调用 PlatformWindow。
// 去重: 平台窗口上报的是原始值(可以重复), 尺寸、状态、屏幕框、控制按钮的"与上次相同则不转发"只在这里做。
// 引擎附着前收到的 metrics/focus/可见性会被缓存, 附着时补发。
// 多视图: 每个窗口一个 WindowController, 以 view_id 绑定到 Shell。
// 线程约定: momo::Object, 归属 platform 线程, 仅在该线程使用。

#include <memory>
#include <optional>

#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_integration.h"
#include "embedder/common/platform_window.h"
#include "embedder/common/window_channel.h"
#include "engine/shell/shell.h"

namespace momo::embedder {

class WindowController final : public Object,
                               public WindowDelegate,
                               public engine::Shell::Delegate {
 public:
  // 需要窗口注册表(WindowManager)做决定的事件。platform 线程回调, 带视图 id。
  class Client {
   public:
    // 用户请求关闭窗口(原生关闭已被拦截): 由 Client 决定是否以及何时真正关闭。
    virtual void OnWindowCloseRequested(engine::platform::ViewId view) = 0;
    // 窗口获得/失去系统焦点(IME 会话交给获得焦点的窗口)。
    virtual void OnWindowFocusChanged(engine::platform::ViewId view, bool focused) = 0;
    virtual void OnWindowTextInput(engine::platform::ViewId view, const TextInputEvent& event) = 0;

   protected:
    ~Client() = default;
  };

  // 创建平台窗口(不显示)及其控制器。client 必须比返回的对象活得久。
  static StatusOr<std::unique_ptr<WindowController>> Create(PlatformIntegration& platform,
                                                           const WindowOptions& options, Client& client,
                                                           engine::platform::ViewId view_id);
  ~WindowController() override;  // 销毁窗口(须先 DetachEngine)

  PlatformWindow& window() { return *window_; }

  // 绑定/解绑引擎。events: 窗口事件交给 JS 的通道, 在 DetachEngine 之前保持有效。
  // DetachEngine 返回后 surface 不再被 raster 线程使用, 也不再向通道发通知。
  void AttachEngine(engine::Shell& shell, WindowChannel& events);
  void DetachEngine();
  engine::platform::ViewId view_id() const { return view_id_; }
  const WindowStateFlags& state() const { return state_; }

  // WindowDelegate
  void OnMetricsChanged(const platform::ViewportMetrics& metrics) override;
  void OnPointerPacket(platform::PointerDataPacket packet) override;
  void OnKeyData(const platform::KeyData& data) override;
  bool OnCloseRequested() override;
  void OnFocusChanged(bool focused) override;
  void OnWindowStateChanged(const WindowStateFlags& state) override;
  void OnTextInput(const TextInputEvent& event) override;
  void OnPlatformViewFocused(int64_t view_id) override;
  void OnWindowFrameChanged(const Rect& screen_frame) override;
  void OnWindowControlsChanged(const std::optional<Rect>& rect) override;

  // engine::Shell::Delegate
  void* GetNativeWindowHandle() override;
  void OnPlatformViewCreated(int64_t view_id, void* native_view) override;
  void OnPlatformViewDisposed(int64_t view_id) override;
  void OnPlatformViewFocusRequested(int64_t view_id) override;
  void OnFirstFramePresented() override;

 private:
  WindowController(Client& client, engine::platform::ViewId view_id);

  bool ShouldRender() const;
  void UpdateRendering();

  Client& client_;
  const engine::platform::ViewId view_id_;
  // 只在 Create 期间为空: 平台窗口在自己的构造过程中就可能回调(定位、布局), 那时的回调只更新下面的缓存。
  std::unique_ptr<PlatformWindow> window_;
  engine::Shell* shell_ = nullptr;      // 非拥有;由 Application 拥有
  WindowChannel* events_ = nullptr;     // 非拥有;与 shell_ 同时绑定/解绑

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
