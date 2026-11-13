#pragma once

// WindowManager: 宿主的窗口注册表(docs/01 §3)。拥有全部 WindowController(视图 id → 窗口), 负责窗口的创建、
// 绑定引擎、关闭询问、焦点与输入法路由。它不决定应用是否退出: 最后一个窗口关闭只通知 Observer
// (Application 据此发 momo/app windowAllClosed, 由 JS 决定)。
//
// 所有窗口对等: 启动窗口是隐式视图 0(JS bundle 执行前即存在, runApp 直接渲染), 其他窗口由 JS 经
// momo/window create 打开;任何窗口都可以关闭。
// 线程约定: momo::Object, 归属 platform 线程, 仅在该线程使用。

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "base/closure.h"
#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"
#include "embedder/common/platform_channels.h"
#include "embedder/common/platform_integration.h"
#include "embedder/common/window_channel.h"
#include "embedder/common/window_controller.h"
#include "engine/shell/shell.h"

namespace momo::embedder {

class WindowManager final : public Object,
                            public WindowChannel::Host,
                            private WindowController::Client {
 public:
  class Observer {
   public:
    // 最后一个窗口已关闭(不含热重启与关闭全部窗口时的中间状态)。在关闭之后的安全点回调;期间又打开了窗口
    // 则不回调。
    virtual void OnAllWindowsClosed() = 0;

   protected:
    ~Observer() = default;
  };

  // platform 与 observer 必须比本对象活得久。startup_spec: 启动窗口的规格, 也是其他窗口的默认值(闪屏底色等)。
  WindowManager(PlatformIntegration& platform, WindowSpec startup_spec, Observer& observer);
  ~WindowManager() override;  // DestroyAll()

  // 创建并显示启动窗口(隐式视图 0)。引擎就绪后由 AttachEngine 绑定。
  Status OpenStartupWindow();

  // 引擎就绪: 绑定已打开的全部窗口, 之后新开的窗口创建即绑定。通道用于把窗口事件交给 JS。
  // 三者都必须在 DetachEngine 之前保持有效。
  void AttachEngine(engine::Shell& shell, WindowChannel& window_channel, PlatformChannels& platform_channels);
  // 全部窗口解绑引擎(raster 不再使用其 surface), 并断开通道。
  void DetachEngine();
  // 销毁全部窗口(须先 DetachEngine)。
  void DestroyAll();

  // 退出流程: 逐个询问窗口(closeRequested), 允许的关闭。done(all_closed) 在全部回复后于 platform 线程回调。
  void RequestCloseAll(MoveOnlyFunction<void(bool all_closed)> done);
  // --dev 热重启: 关闭旧 JS 打开的窗口;keep_startup_window 时保证启动窗口存在(之前被关闭则重新打开)。
  void ResetForHotRestart(bool keep_startup_window);

  size_t count() const { return windows_.size(); }
  bool empty() const { return windows_.empty(); }
  bool has_visible_window() const;
  WindowController* window(engine::platform::ViewId view_id) const;
  // 任一打开着的窗口(id 最小者;没有窗口时为空)。
  WindowController* front_window() const;
  std::vector<engine::platform::ViewId> view_ids() const;
  // 输入法会话所在的连接: 获得焦点的窗口(没有则任一窗口)。
  TextInputConnection* FocusedTextInput();

  // WindowChannel::Host
  StatusOr<engine::platform::ViewId> CreateWindow(const WindowOptions& options) override;
  void CloseWindow(engine::platform::ViewId view_id) override;
  void RequestCloseWindow(engine::platform::ViewId view_id) override;
  PlatformWindow* FindWindow(engine::platform::ViewId view_id) override;

 private:
  StatusOr<std::unique_ptr<WindowController>> NewWindow(engine::platform::ViewId view_id,
                                                        const WindowSpec& spec);
  // 登记窗口;show 时显示;引擎已就绪则绑定。
  void Open(std::unique_ptr<WindowController> controller, bool show);
  void Attach(WindowController& controller);
  // report_all_closed: 关闭最后一个窗口时通知 Observer(热重启时不通知)。
  void Close(engine::platform::ViewId view_id, bool report_all_closed);

  // WindowController::Client
  void OnWindowCloseRequested(engine::platform::ViewId view) override;
  void OnWindowStateChanged(engine::platform::ViewId view, const WindowStateFlags& state) override;
  void OnWindowFocusChanged(engine::platform::ViewId view, bool focused) override;
  void OnWindowTextInput(engine::platform::ViewId view, const TextInputEvent& event) override;
  void OnWindowFrameChanged(engine::platform::ViewId view, const Rect& screen_frame) override;
  void OnWindowControlsChanged(engine::platform::ViewId view, const std::optional<Rect>& controls) override;
  void OnWindowFirstFrame(engine::platform::ViewId view) override;

  PlatformIntegration& platform_;
  const WindowSpec startup_spec_;
  Observer& observer_;

  engine::Shell* shell_ = nullptr;
  WindowChannel* window_channel_ = nullptr;
  PlatformChannels* platform_channels_ = nullptr;

  std::map<engine::platform::ViewId, std::unique_ptr<WindowController>> windows_;
  engine::platform::ViewId next_view_id_ = engine::platform::kImplicitViewId + 1;
  std::optional<engine::platform::ViewId> focused_window_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WindowManager);
};

}  // namespace momo::embedder
