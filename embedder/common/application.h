#pragma once

// Application: 进程级生命周期根对象(docs/01 §3)。每进程由平台入口创建一个。
//
// 状态机: NotRunning → Launching → Running ⇄ Suspended → Terminating
//                         └──────────(启动失败)──────────────┘
// Launching 跨越事件循环: Launch() 建好窗口与引擎、异步启动 JS 运行时后就返回, 主线程照常泵消息;
// bundle 执行完的结果回到 platform 线程后才进入 Running(失败则以退出码 1 有序退出)。
// Suspended 只表示应用整体被隐藏(macOS Cmd+H / applicationDidHide):与窗口无关——窗口的最小化、隐藏只影响
// 该窗口的视图是否出帧(引擎按视图处理)。没有窗口也是合法状态: JS 照常运行, 不出帧。
//
// 职责: 持有 PlatformIntegration、OS 主线程的 EventLoop(经 base::NativeMessagePump 跑原生消息循环)、
//       ui/raster/io 线程、引擎 Shell、窗口注册表(WindowManager);注册宿主通道(momo/app、momo/window、
//       平台服务);退出状态机;按 docs/01 §4 严格有序关闭。
// 退出(同 Electron 的 app.quit / app.exit;没有"直接终止"的入口):
//   Quit()  —— Cmd+Q、菜单退出、JS app.quit():  beforeQuit → 逐个询问窗口 closeRequested 并关闭 → willQuit →
//              Shutdown;任一步被 JS 取消即中止。启动期间(bundle 还在执行)同样走这条流程: 询问排在 bundle
//              之后, JS 没有处理器时视为允许。
//   Exit(c) —— JS app.exit(c)、process.exit(c)、启动失败: 不询问, 关闭全部窗口 → Shutdown, 退出码 c。
//   Shutdown(内部): 进入 Terminating(JS 收到这一状态, 相当于 Electron 的 'quit' 事件), 请求消息循环结束。
//              这里不销毁任何东西: JS 照常运行到循环结束。
//   最后一个窗口关闭不退出: 只发 momo/app windowAllClosed, 由 JS 决定。
// 销毁(TearDown): 消息循环结束之后(Run 返回前)按依赖逐级进行, 引擎最先(docs/01 §4):
//   引擎(平台视图 → JS 运行时 → GPU 资源, 见 Shell::Shutdown)→ 宿主通道 → 窗口 → 线程 join。
//   销毁要与 ui 线程汇合。JS 不返回(等待调试器连接、脚本死循环)时退出就停在这里等它——不强行终止 JS,
//   也不强杀进程。
// 线程约定: momo::Object, 归属 platform 线程(OS 主线程);除 state() 外所有方法仅在该线程调用。

#include <memory>
#include <string_view>

#include "base/event_loop.h"
#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"
#include "embedder/common/app_channel.h"
#include "embedder/common/app_config.h"
#include "embedder/common/platform_channels.h"
#include "embedder/common/platform_integration.h"
#include "embedder/common/window_channel.h"
#include "embedder/common/window_manager.h"
#include "engine/common/task_runners.h"
#include "engine/shell/shell.h"
#include "engine/shell/thread_host.h"

namespace momo::embedder {

class Application final : public Object, private WindowManager::Observer, private AppChannel::Host {
 public:
  enum class State { kNotRunning, kLaunching, kRunning, kSuspended, kTerminating };

  // 关闭步骤, 按发生顺序, 与 docs/01 §4 一一对应。
  enum class ShutdownStep {
    kMessageLoopQuit,  // 0) 请求 platform EventLoop 结束(排空任务、清理 DeleteLater 对象后退出原生循环)
    kEngineShutdown,   // 1+2) ui: 销毁 JsRuntime, 再停帧/卸扩展;raster: 释放 GPU 资源
    kWindowDestroyed,  // 3) 销毁 RenderSurface 与 PlatformWindow
    kThreadsJoined,    // 4) join ui/raster/io
  };

  // 可选观察者(日志、测试)。在 platform 线程回调。
  class Observer {
   public:
    virtual void OnStateChanged(State from, State to) {}
    virtual void OnShutdownStep(ShutdownStep step) {}

   protected:
    ~Observer() = default;
  };

  struct Options {
    AppConfig config;
    // 资源根目录(Mac: NSBundle resourcePath;Win: exe 目录)。
    std::filesystem::path resources_dir;
    // 本应用的用户缓存目录(Mac: ~/Library/Caches/<bundle id>;Win: %LOCALAPPDATA%\<exe 名>), 由平台入口
    // 给出;V8 code cache 放在它的 v8/ 下。为空 = 不缓存。
    std::filesystem::path cache_dir;
    Observer* observer = nullptr;  // 非拥有, 必须比 Application 活得久
  };

  // 启动时序第 3 步: 接管平台集成, 创建 TaskRunners(platform 绑定当前线程, ui/raster/io 各建线程)。
  static StatusOr<std::unique_ptr<Application>> Create(
      Options options, std::unique_ptr<PlatformIntegration> platform);
  ~Application() override;  // 尚未销毁则在这里完成(Shutdown + TearDown)

  // 启动时序第 4–5 步: 创建并显示启动窗口(config.startup_window)→ 创建引擎(Shell::Create, 不执行 JS)→
  // 注册宿主通道、绑定窗口 → 异步启动 JS 运行时(Shell::Run)。不等待 bundle 执行: 返回时状态仍是 Launching,
  // 结果稍后在事件循环里到达(成功 → Running;bundle 加载/执行失败 → Exit(1), Run() 返回 1)。
  // 返回值只反映同步部分(窗口、资源解析、引擎创建)的错误;这类失败已完成有序销毁, 状态为 Terminating。
  Status Launch();

  // 启动时序第 7 步: 运行 platform EventLoop(原生消息循环)直到退出流程请求它结束, 然后逐级销毁(TearDown)。
  // 返回进程退出码。Launch 之后即可调用(不必等 JS 运行时就绪)。
  int Run();

  // 请求退出(可被 JS 取消;重复请求合并)。
  void Quit() override;
  // 不询问: 关闭全部窗口, 以 exit_code 退出。幂等。
  void Exit(int exit_code) override;

  // ---- 平台入口的应用级事件 ----
  // 应用整体隐藏/显示(Mac applicationDidHide/Unhide): 隐藏即 Suspended。
  void OnAppVisibilityChanged(bool visible);
  // 应用成为/不再是前台应用(Mac applicationDidBecomeActive/ResignActive, Win WM_ACTIVATEAPP)。
  void OnAppActiveChanged(bool active);
  // 用户重新激活应用(Mac 点击 Dock 图标:applicationShouldHandleReopen)。
  void OnReopen(bool has_visible_windows);

  State state() const { return state_; }
  bool is_quitting() const { return quitting_; }
  int exit_code() const { return exit_code_; }
  const engine::TaskRunners& task_runners() const { return task_runners_; }
  PlatformIntegration& platform() { return *platform_; }
  engine::Shell* shell() const { return shell_.get(); }
  WindowManager& windows() { return *window_manager_; }

  static bool IsValidTransition(State from, State to);
  // JS 可见的状态名: launching / running / suspended / terminating(未启动为 notRunning)。
  static std::string_view StateName(State state);

 private:
  Application(Options options, std::unique_ptr<PlatformIntegration> platform,
              std::shared_ptr<EventLoop> platform_loop);

  bool TransitionTo(State to);
  void UpdateSuspended();
  void NotifyShutdownStep(ShutdownStep step);
  // 解析 bundle/资源 → 创建 Shell → 注册宿主通道 → 绑定窗口 → Shell::Run(异步)。
  Status LaunchEngine();
  // Shell::Run 的结果(platform 线程): 完成 Launching → Running, 或启动失败退出。
  void OnRuntimeStarted(Status status);

  // ---- 退出流程(Quit) ----
  void QuitCloseWindows();
  void QuitWillQuit();
  void AbortQuit(std::string_view reason);
  // 退出流程的最后一步(Quit 与 Exit 汇合于此): 进入 Terminating, 请求消息循环结束。不销毁任何东西。幂等。
  void Shutdown();
  // 消息循环结束后逐级销毁, JS 环境最先(见文件头)。同步, 幂等。
  void TearDown();

  // WindowManager::Observer
  void OnAllWindowsClosed() override;

  // AppChannel::Host
  Status HideApp() override;
  Status ShowApp() override;
  Status FocusApp(bool steal) override;
  engine::EncodableValue GetAppState() override;

  Options options_;
  State state_ = State::kNotRunning;
  int exit_code_ = 0;
  bool app_visible_ = true;
  bool active_ = false;
  bool quitting_ = false;   // Quit 流程进行中(在等 JS 的答复)
  bool torn_down_ = false;

  // 成员声明顺序即逆序析构顺序: platform_ 最后析构(platform_loop_ 的 pump 引用其 native loop;
  // pump 析构不再触碰它, 且循环结束后不再投递)。
  std::unique_ptr<PlatformIntegration> platform_;
  std::shared_ptr<EventLoop> platform_loop_;  // 绑定 OS 主线程
  std::unique_ptr<engine::ThreadHost> thread_host_;  // ui/raster/io
  engine::TaskRunners task_runners_;
  // 全部窗口。先于 shell_ 声明: 后于 Shell 析构(raster 停用 surface 之后才销毁窗口)。
  std::unique_ptr<WindowManager> window_manager_;
  std::unique_ptr<engine::Shell> shell_;
  // 宿主通道: Shell 创建后、JS 运行时启动前注册;JS 运行时销毁(Shell 关闭)之后移除。
  std::unique_ptr<AppChannel> app_channel_;
  std::unique_ptr<WindowChannel> window_channel_;
  std::unique_ptr<PlatformChannels> platform_channels_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Application);
};

}  // namespace momo::embedder
