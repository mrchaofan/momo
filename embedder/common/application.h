#pragma once

// Application: 进程级生命周期根对象(docs/01 §3)。每进程由平台入口创建一个。
//
// 状态机: NotRunning → Launching → Running ⇄ Suspended → Terminating
//                         └──────────(启动失败)──────────────┘
// Suspended 只表示应用整体被隐藏(macOS Cmd+H / applicationDidHide):与窗口无关——窗口的最小化、隐藏只影响
// 该窗口的视图是否出帧(引擎按视图处理)。没有窗口也是合法状态: JS 照常运行, 不出帧。
//
// 职责: 持有 PlatformIntegration、OS 主线程的 EventLoop(经 base::NativeMessagePump 跑原生消息循环)、
//       ui/raster/io 线程、引擎 Shell、窗口注册表(WindowManager);注册宿主通道(momo/app、momo/window、
//       平台服务);退出状态机;按 docs/01 §4 严格有序关闭。
// 退出:
//   Quit()  —— Cmd+Q、菜单退出、JS app.quit():  beforeQuit → 逐个询问窗口 closeRequested → willQuit →
//              有序关闭;任一步被 JS 取消即中止(同 Electron app.quit)。
//   Exit(c) —— JS app.exit(c)、process.exit(c): 不询问, 有序关闭, 退出码 c。
//   最后一个窗口关闭不退出: 只发 momo/app windowAllClosed, 由 JS 决定。
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

  // 关闭步骤, 与 docs/01 §4 一一对应。
  enum class ShutdownStep {
    kEngineShutdown,   // 1+2) ui: 停帧/卸扩展/销毁 JsRuntime;raster: 释放 GPU 资源
    kWindowDestroyed,  // 3) 销毁 RenderSurface 与 PlatformWindow
    kThreadsJoined,    // 4) join ui/raster/io
    kMessageLoopQuit,  //    请求 platform EventLoop 结束(排空任务、清理 DeleteLater 对象后退出原生循环)
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
    Observer* observer = nullptr;  // 非拥有, 必须比 Application 活得久
    // 退出流程中每次询问 JS 后若在此时间内无回复(ui 线程卡死), 强制退出进程(无法有序关闭: 关闭需要
    // 与 ui 线程同步汇合)。<= 0 关闭超时机制。
    TimeDelta close_timeout = std::chrono::seconds(5);
    // 强制退出的实现(测试可替换);默认 std::_Exit。
    std::function<void(int exit_code)> force_exit;
    // --dev 时轮询 bundle 修改时间的间隔(变化即 hot restart);<= 0 关闭。
    TimeDelta hot_restart_poll = std::chrono::milliseconds(500);
  };

  // 启动时序第 3 步: 接管平台集成, 创建 TaskRunners(platform 绑定当前线程, ui/raster/io 各建线程)。
  static StatusOr<std::unique_ptr<Application>> Create(
      Options options, std::unique_ptr<PlatformIntegration> platform);
  ~Application() override;  // 若尚未关闭则执行 Terminate()

  // 启动时序第 4–5 步: 创建并显示启动窗口(config.startup_window)→ 启动引擎(V8 + bundle)→ 绑定窗口。
  // 失败时已完成有序清理, 状态为 Terminating。
  Status Launch();

  // 启动时序第 7 步: 运行 platform EventLoop(原生消息循环), 直到 Terminate 后排空。返回进程退出码。
  int Run();

  // 请求退出(可被 JS 取消;重复请求合并)。
  void Quit() override;
  // 不询问, 在下一个安全点有序关闭(可能在宿主通道处理器的调用栈里被调用)。
  void Exit(int exit_code) override;
  // 立即有序关闭并退出消息循环。幂等。
  void Terminate(int exit_code = 0);

  // ---- 平台入口的应用级事件 ----
  // 应用整体隐藏/显示(Mac applicationDidHide/Unhide): 隐藏即 Suspended。
  void OnAppVisibilityChanged(bool visible);
  // 应用成为/不再是前台应用(Mac applicationDidBecomeActive/ResignActive, Win WM_ACTIVATEAPP)。
  void OnAppActiveChanged(bool active);
  // 用户重新激活应用(Mac 点击 Dock 图标:applicationShouldHandleReopen)。
  void OnReopen(bool has_visible_windows);

  State state() const { return state_; }
  bool is_active() const { return active_; }
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
  // 解析 bundle/资源 → 创建 Shell(同步等待 JS 运行时与 raster 就绪)→ 注册宿主通道 → 绑定窗口。
  Status LaunchEngine();
  // --dev: 定时检查 bundle 修改时间, 变化时关闭旧 JS 打开的窗口(按配置保证启动窗口存在)并 hot restart。
  void PollBundleForChanges();

  // ---- 退出流程(Quit) ----
  void QuitCloseWindows();
  void QuitWillQuit();
  void AbortQuit(std::string_view reason);
  // 每次询问 JS 开一轮看门狗: 超时未回复(ui 线程卡死)即强制退出。EndAsk 返回该回复是否仍有效。
  uint64_t BeginAsk();
  bool EndAsk(uint64_t serial);

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
  bool quitting_ = false;
  uint64_t ask_serial_ = 0;
  bool ask_pending_ = false;
  std::filesystem::path watched_bundle_;
  std::filesystem::file_time_type watched_bundle_mtime_;

  // 成员声明顺序即逆序析构顺序: platform_ 最后析构(platform_loop_ 的 pump 引用其 native loop;
  // pump 析构不再触碰它, 且循环结束后不再投递)。
  std::unique_ptr<PlatformIntegration> platform_;
  std::shared_ptr<EventLoop> platform_loop_;  // 绑定 OS 主线程
  std::unique_ptr<engine::ThreadHost> thread_host_;  // ui/raster/io
  engine::TaskRunners task_runners_;
  // 全部窗口。先于 shell_ 声明: 后于 Shell 析构(raster 停用 surface 之后才销毁窗口)。
  std::unique_ptr<WindowManager> window_manager_;
  std::unique_ptr<engine::Shell> shell_;
  // 宿主通道: Shell 创建时(JS bundle 执行前)注册, Shell 关闭前移除(先于 shell_ 析构)。
  std::unique_ptr<AppChannel> app_channel_;
  std::unique_ptr<WindowChannel> window_channel_;
  std::unique_ptr<PlatformChannels> platform_channels_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Application);
};

}  // namespace momo::embedder
