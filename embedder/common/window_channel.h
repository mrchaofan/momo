#pragma once

// WindowChannel: 宿主的窗口管理通道 momo/window(ARCHITECTURE.md §3.10)。窗口外观与行为属于宿主, 所以由 embedder 直接在
// 引擎的消息通道上注册处理器, JS 与宿主之间往返, 不经过引擎(对标 Chromium views::Widget ⇄ NativeWidget)。
// 框架里每个窗口是一个 Widget 对象, 它的方法与事件一一对应这里的协议。
//
// momo/window(StandardMethodCodec;逻辑点;view 为视图 id):
//   JS → 宿主:
//     create {title?, width, height, minWidth?, minHeight?, x?, y?, nonActivating?, resizable?, show?}
//                                         → 窗口快照(见下)加 view
//     close {view}                        立即关闭(不询问 closeRequested)
//     requestClose {view}                 与用户点关闭按钮相同: 先询问 closeRequested, 允许后关闭
//     getState {view} → 窗口快照: {frame: [x, y, w, h] | null, controls: [x, y, w, h] | null, focused,
//                        state: {visible, minimized, maximized, fullscreen}}
//     show {view} / hide {view} / minimize {view} / maximize {view} / restore {view} / activate {view}
//     setTitle {view, title} / setMinSize {view, width, height} / setCursor {view, cursor}
//     setAlwaysOnTop {view, on} / setPosition {view, x, y}
//     setResizable {view, resizable}     用户能否拖动边框缩放窗口
//     setResizeBorder {view, reset?, left?, top?, right?, bottom?}   窗口内侧的系统缩放边框(reset: 各边先恢复
//                                         默认值;未给出的边保持不变)
//     setTitleBar {view, hidden, height?}
//     startDrag {view, sequenceId}        以进行中的 momo 按下序列开始系统窗口拖动(业务层决定哪里可拖)
//     startMoveLoop {view, x, y} / endMoveLoop {view}   宿主驱动的窗口跟随指针(x, y = 抓取偏移)
//     capturePointer {view, sequenceId} → bool   进行中的 momo 按下序列改由该窗口接收
//   宿主 → JS:
//     frameChanged {view, frame: [x, y, w, h]}
//     controlsChanged {view, controls: [x, y, w, h] | null}
//     stateChanged {view, state: {visible, minimized, maximized, fullscreen}}
//     focusChanged {view, focused}        窗口获得/失去系统焦点
//     readyToShow {view}                  第一帧已呈现(以 show: false 创建的窗口据此显示)
//     closed {view}                       窗口已关闭(视图已从 momo:ui viewports 移除)
//     closeRequested {view} → bool        用户点了关闭按钮(或 requestClose / 退出应用);false 取消
// 状态只有一个方向: JS 拿到一份快照(自己创建的窗口在 create 的回复里, 宿主启动时创建的窗口查一次 getState),
// 之后只靠上面的通知更新。回复与通知都从 platform 线程按序投递, JS 按到达顺序应用即可。
// 平台不支持的方法回复 "unimplemented"(没有另外的能力声明)。
// 线程约定: 归属 platform 线程;处理器在 platform 线程运行。

#include <memory>
#include <optional>

#include "base/closure.h"
#include "base/macros.h"
#include "base/status.h"
#include "base/task_runner.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/host_channel.h"
#include "embedder/common/platform_window.h"
#include "embedder/common/window_types.h"
#include "engine/common/message_channel_hub.h"
#include "engine/platform/view.h"

namespace momo::embedder {

class WindowChannel final {
 public:
  // 窗口的拥有者(WindowManager)。platform 线程回调。
  class Host {
   public:
    // 创建窗口并绑定到引擎(options.show 时显示), 返回新视图 id。
    virtual StatusOr<platform::ViewId> CreateWindow(const WindowOptions& options) = 0;
    // 解绑并销毁窗口(未知 id 忽略)。
    virtual void CloseWindow(platform::ViewId view) = 0;
    // 先询问 JS(closeRequested)再关闭。
    virtual void RequestCloseWindow(platform::ViewId view) = 0;
    virtual PlatformWindow* FindWindow(platform::ViewId view) = 0;
    // 窗口是否持有系统焦点(未知 id 为 false)。
    virtual bool IsWindowFocused(platform::ViewId view) = 0;

   protected:
    ~Host() = default;
  };

  static constexpr const char kName[] = "momo/window";

  // hub 上注册处理器(在 platform_runner 上运行)。host 必须比本对象活得久。
  WindowChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                std::shared_ptr<TaskRunner> platform_runner, Host* host);

  // ---- 宿主 → JS ----
  void NotifyFrameChanged(platform::ViewId view, const Rect& frame);
  void NotifyControlsChanged(platform::ViewId view, const std::optional<Rect>& controls);
  void NotifyStateChanged(platform::ViewId view, const WindowStateFlags& state);
  void NotifyReadyToShow(platform::ViewId view);
  void NotifyFocusChanged(platform::ViewId view, bool focused);
  void NotifyClosed(platform::ViewId view);
  // 询问 JS 是否允许关闭窗口。reply 在 platform 线程回调(JS 无处理器、出错或运行时已不在时为 true)。
  void RequestClose(platform::ViewId view, MoveOnlyFunction<void(bool allow)> reply);

  // 处理一次 JS 调用(测试可直接调用)。
  StatusOr<engine::EncodableValue> HandleCall(const engine::MethodCall& call) {
    return channel_.Dispatch(call);
  }

  static engine::EncodableValue StateValue(const WindowStateFlags& state);

 private:
  // 需要窗口的方法: 解析 view, 未知视图回复 notFound。
  using WindowHandler =
      std::function<StatusOr<engine::EncodableValue>(PlatformWindow& window, const engine::EncodableValue& args)>;
  void RegisterWindowMethod(std::string method, WindowHandler handler);
  void RegisterMethods();
  // 窗口当前状态的快照(create / getState 的回复)。
  engine::EncodableMap Snapshot(platform::ViewId view, PlatformWindow& window);

  Host* const host_;
  HostChannel channel_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WindowChannel);
};

}  // namespace momo::embedder
