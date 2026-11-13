#pragma once

// AppChannel: 宿主的应用通道 momo/app(docs/03 §2)。应用生命周期属于宿主(不经引擎);框架的 app 对象的方法与
// 事件一一对应这里的协议。新增应用接口 = PlatformApp 加能力 + 这里 Register 一个方法 + 框架 app 加方法。
//
// momo/app(StandardMethodCodec):
//   JS → 宿主:
//     quit                      请求退出: beforeQuit → 逐个询问窗口 closeRequested → willQuit → 有序退出;任一步取消即中止
//     exit {code?}              立即有序退出(不询问), 退出码 code(缺省 0)
//     hide / show               隐藏 / 显示整个应用
//     focus {steal?}            把应用带到前台
//     getState → {state, active, capabilities: [string]}
//   宿主 → JS:
//     stateChanged {state}      "launching" | "running" | "suspended" | "terminating"(suspended = 应用被隐藏)
//     activeChanged {active}    应用成为/不再是前台应用
//     beforeQuit → bool         退出流程开始;false 取消
//     willQuit → bool           窗口已全部关闭, 即将退出;false 取消(应用无窗口继续运行)
//     windowAllClosed           最后一个窗口已关闭(退出流程中不发);应用默认继续运行
//     activate {hasVisibleWindows}   用户重新激活应用(macOS 点击 Dock 图标)
// 线程约定: 归属 platform 线程;处理器在 platform 线程运行。

#include <memory>
#include <string_view>

#include "base/closure.h"
#include "base/macros.h"
#include "base/status.h"
#include "base/task_runner.h"
#include "embedder/common/host_channel.h"
#include "engine/common/message_channel_hub.h"

namespace momo::embedder {

class AppChannel final {
 public:
  // 应用(Application)。platform 线程回调。
  class Host {
   public:
    virtual void Quit() = 0;
    virtual void Exit(int exit_code) = 0;
    virtual Status HideApp() = 0;
    virtual Status ShowApp() = 0;
    virtual Status FocusApp(bool steal) = 0;
    // {state, active, capabilities}
    virtual engine::EncodableValue GetAppState() = 0;

   protected:
    ~Host() = default;
  };

  static constexpr const char kName[] = "momo/app";

  AppChannel(std::shared_ptr<engine::MessageChannelHub> hub, std::shared_ptr<TaskRunner> platform_runner,
             Host* host);

  // ---- 宿主 → JS ----
  void NotifyState(std::string_view state);
  void NotifyActive(bool active);
  void NotifyWindowAllClosed();
  void NotifyActivate(bool has_visible_windows);
  void AskBeforeQuit(MoveOnlyFunction<void(bool allow)> reply);
  void AskWillQuit(MoveOnlyFunction<void(bool allow)> reply);

  StatusOr<engine::EncodableValue> HandleCall(const engine::MethodCall& call) {
    return channel_.Dispatch(call);
  }

 private:
  Host* const host_;
  HostChannel channel_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(AppChannel);
};

}  // namespace momo::embedder
