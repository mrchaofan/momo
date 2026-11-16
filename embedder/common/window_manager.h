#pragma once

// WindowManager: 宿主的窗口注册表(ARCHITECTURE.md §3.10)。拥有全部 WindowController(视图 id → 窗口)与窗口通道
// momo/window, 负责窗口的创建、关闭询问与焦点归属。它不决定应用是否退出: 最后一个窗口关闭只通知 Observer
// (Application 据此发 momo/app windowAllClosed, 由 JS 决定)。
// 引擎先于窗口: 本对象在引擎创建之后构造, 每个窗口创建即绑定为引擎的视图——没有"有窗口、没有引擎"的状态。
// 帧时钟: 应用只有一个 VsyncSource(构造时创建, 交给 Shell 持有);它跟随 id 最小的窗口所在的
// 显示器(视图 0 优先), 窗口打开/关闭时重新选择。
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
#include "embedder/common/platform_integration.h"
#include "embedder/common/text_input_channel.h"
#include "embedder/common/window_channel.h"
#include "embedder/common/window_controller.h"
#include "engine/shell/shell.h"

namespace momo::embedder {

class WindowManager final : public Object, public WindowChannel::Host {
 public:
  class Observer {
   public:
    // 最后一个窗口已关闭(不含退出流程里关闭全部窗口的中间状态)。在关闭之后的安全点回调;期间又打开了窗口
    // 则不回调。
    virtual void OnAllWindowsClosed() = 0;

   protected:
    ~Observer() = default;
  };

  // 四个引用都必须比本对象活得久。startup_options: 启动窗口的参数, 也是其他窗口的默认值(标题、闪屏底色)。
  // 注册 momo/window, 创建应用的 VsyncSource 交给 shell。
  WindowManager(PlatformIntegration& platform, WindowOptions startup_options, Observer& observer,
                engine::Shell& shell, TextInputChannel& text_input);
  ~WindowManager() override;  // DetachEngine() + DestroyAll()

  // 创建并显示启动窗口(隐式视图 0;早于 JS bundle 执行)。
  Status OpenStartupWindow();

  // 全部窗口解绑引擎(raster 不再使用其 surface), 之后不再向 JS 发通知。引擎关闭之后、销毁窗口之前调用。
  void DetachEngine();
  // 销毁全部窗口(须先 DetachEngine)。
  void DestroyAll();

  // 不询问, 关闭全部窗口(不报告 OnAllWindowsClosed)。
  void CloseAll();
  // 退出流程: 逐个询问窗口(closeRequested), 允许的关闭。done(all_closed) 在全部回复后于 platform 线程回调。
  void RequestCloseAll(MoveOnlyFunction<void(bool all_closed)> done);

  size_t count() const { return windows_.size(); }
  bool empty() const { return windows_.empty(); }
  bool has_visible_window() const;
  WindowController* window(engine::platform::ViewId view_id) const;
  // 任一打开着的窗口(id 最小者;没有窗口时为空)。
  WindowController* front_window() const;
  std::vector<engine::platform::ViewId> view_ids() const;
  // 获得系统焦点的窗口(没有时为空)。
  WindowController* focused_window() const;
  // 输入法会话所在的连接: 获得焦点的窗口(没有则任一窗口)。
  TextInputConnection* FocusedTextInput();

  // WindowChannel::Host
  StatusOr<engine::platform::ViewId> CreateWindow(const WindowOptions& options) override;
  void CloseWindow(engine::platform::ViewId view_id) override;
  void RequestCloseWindow(engine::platform::ViewId view_id) override;
  PlatformWindow* FindWindow(engine::platform::ViewId view_id) override;
  bool IsWindowFocused(engine::platform::ViewId view_id) override;

 private:
  // 创建窗口(绑定为引擎的视图)并登记;options.show 时显示。
  Status Open(engine::platform::ViewId view_id, const WindowOptions& options);
  // 帧时钟跟随的窗口: id 最小者(视图 0 优先);没有窗口时为主显示器。
  void UpdateVsyncWindow();
  // report_all_closed: 关闭最后一个窗口时通知 Observer(退出流程里不通知)。
  void Close(engine::platform::ViewId view_id, bool report_all_closed);

  PlatformIntegration& platform_;
  const WindowOptions startup_options_;
  Observer& observer_;

  engine::Shell& shell_;
  TextInputChannel& text_input_;
  WindowChannel channel_;
  bool detached_ = false;  // DetachEngine 之后: 引擎已关闭, 不再开窗口、不再通知 JS

  std::map<engine::platform::ViewId, std::unique_ptr<WindowController>> windows_;
  engine::platform::ViewId next_view_id_ = engine::platform::kImplicitViewId + 1;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WindowManager);
};

}  // namespace momo::embedder
