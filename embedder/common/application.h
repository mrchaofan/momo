#pragma once

// Application: 进程级生命周期根对象(docs/01 §3)。每进程由平台入口创建一个。
//
// 状态机: NotRunning → Launching → Running ⇄ Suspended → Terminating
//                         └──────────(启动失败)──────────────┘
// 职责: 持有 PlatformIntegration、OS 主线程的 EventLoop(经 base::NativeMessagePump 跑原生消息循环)、
//       ui/raster/io 线程、主窗口与引擎 Shell, 以及 JS 经 createView 打开的其他窗口(Shell::HostDelegate);
//       向 JS 分发生命周期事件;处理退出请求(JS 可拦截);按 docs/01 §4 严格有序关闭。
// 线程约定: momo::Object, 归属 platform 线程(OS 主线程);除 state() 外所有方法仅在该线程调用。

#include <map>
#include <memory>
#include <set>
#include <string_view>

#include "base/event_loop.h"
#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"
#include "embedder/common/app_config.h"
#include "embedder/common/platform_integration.h"
#include "embedder/common/window_controller.h"
#include "engine/common/task_runners.h"
#include "engine/shell/shell.h"
#include "engine/shell/thread_host.h"

namespace momo::embedder {

// 多窗口策略: 主窗口(视图 0)关闭 = 退出应用(先关闭其他窗口);其他窗口的关闭按钮询问
// view.onCloseRequested, 允许后只关闭该窗口。所有窗口都最小化(或应用隐藏)时进入 Suspended。
class Application final : public Object, private engine::Shell::HostDelegate {
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
    // 关闭请求发给 JS 后若在此时间内无回复(ui 线程卡死), 强制退出进程(无法有序关闭: 关闭需要
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

  // 启动时序第 4–5 步: 创建并显示主窗口 → 启动引擎(V8 + bundle)→ 绑定窗口。
  // 失败时已完成有序清理, 状态为 Terminating。
  Status Launch();

  // 启动时序第 7 步: 运行 platform EventLoop(原生消息循环), 直到 Terminate 后排空。返回进程退出码。
  int Run();

  // 用户/系统请求退出(关窗、Cmd+Q、WM_CLOSE): 先询问 JS, 允许后 Terminate。重复请求被合并。
  void RequestTerminate();
  // 立即有序关闭并退出消息循环。幂等。
  void Terminate(int exit_code = 0);

  // 应用整体隐藏/显示(Mac applicationDidHide/Unhide)。窗口最小化经 WindowController 进入。
  void OnAppVisibilityChanged(bool visible);

  State state() const { return state_; }
  int exit_code() const { return exit_code_; }
  const engine::TaskRunners& task_runners() const { return task_runners_; }
  PlatformIntegration& platform() { return *platform_; }
  engine::Shell* shell() const { return shell_.get(); }
  WindowController* window_controller() const { return window_controller_.get(); }
  // JS 打开的其他窗口(不含主窗口)。
  size_t extra_window_count() const { return extra_windows_.size(); }
  WindowController* extra_window(engine::platform::ViewId view_id) const;

  static bool IsValidTransition(State from, State to);
  static std::string_view StateName(State state);

 private:
  Application(Options options, std::unique_ptr<PlatformIntegration> platform,
              std::shared_ptr<EventLoop> platform_loop);

  bool TransitionTo(State to);
  void UpdateSuspended();
  void NotifyShutdownStep(ShutdownStep step);
  // 解析 bundle/资源 → 创建 Shell(同步等待 JS 运行时与 raster 就绪)→ 绑定主窗口。
  Status LaunchEngine();
  // --dev: 定时检查 bundle 修改时间, 变化时 Shell::RequestHotRestart。
  void PollBundleForChanges();

  // engine::Shell::HostDelegate(platform 线程)
  Status CreateView(engine::platform::ViewId view_id,
                    const engine::platform::ViewOptions& options) override;
  void CloseView(engine::platform::ViewId view_id) override;
  // 非主窗口的关闭按钮: 询问 JS 后关闭。
  void RequestCloseView(engine::platform::ViewId view_id);
  void OnWindowVisibilityChanged(engine::platform::ViewId view_id, bool visible);

  Options options_;
  State state_ = State::kNotRunning;
  int exit_code_ = 0;
  bool terminate_requested_ = false;
  uint64_t close_request_serial_ = 0;  // 区分不同轮次的关闭请求(超时只针对仍未回复的那一轮)
  std::filesystem::path watched_bundle_;
  std::filesystem::file_time_type watched_bundle_mtime_;
  bool app_visible_ = true;
  std::set<engine::platform::ViewId> minimized_windows_;

  // 成员声明顺序即逆序析构顺序: platform_ 最后析构(platform_loop_ 的 pump 引用其 native loop;
  // pump 析构不再触碰它, 且循环结束后不再投递)。
  std::unique_ptr<PlatformIntegration> platform_;
  std::shared_ptr<EventLoop> platform_loop_;  // 绑定 OS 主线程
  std::unique_ptr<engine::ThreadHost> thread_host_;  // ui/raster/io
  engine::TaskRunners task_runners_;
  std::unique_ptr<WindowController> window_controller_;
  std::map<engine::platform::ViewId, std::unique_ptr<WindowController>> extra_windows_;
  std::unique_ptr<engine::Shell> shell_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Application);
};

}  // namespace momo::embedder
