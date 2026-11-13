#include "embedder/common/application.h"

#include <cstdlib>

#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/native_message_pump.h"
#include "embedder/common/asset_resolver.h"
#include "embedder/common/engine_platform.h"

namespace momo::embedder {

namespace {

constexpr const char kLabel[] = "momo";

}  // namespace

// ---------------------------------------------------------------------------
// 状态机

bool Application::IsValidTransition(State from, State to) {
  switch (from) {
    case State::kNotRunning:
      return to == State::kLaunching || to == State::kTerminating;
    case State::kLaunching:
      return to == State::kRunning || to == State::kTerminating;
    case State::kRunning:
      return to == State::kSuspended || to == State::kTerminating;
    case State::kSuspended:
      return to == State::kRunning || to == State::kTerminating;
    case State::kTerminating:
      return false;
  }
  return false;
}

std::string_view Application::StateName(State state) {
  switch (state) {
    case State::kNotRunning: return "notRunning";
    case State::kLaunching: return "launching";
    case State::kRunning: return "running";
    case State::kSuspended: return "suspended";
    case State::kTerminating: return "terminating";
  }
  return "?";
}

bool Application::TransitionTo(State to) {
  const State from = state_;
  if (!IsValidTransition(from, to)) {
    MOMO_LOG(WARNING) << "Ignoring invalid lifecycle transition " << StateName(from) << " -> "
                      << StateName(to);
    return false;
  }
  state_ = to;
  MOMO_LOG(VERBOSE) << "Lifecycle " << StateName(from) << " -> " << StateName(to);
  if (shell_ != nullptr) {
    shell_->SetAppSuspended(to != State::kRunning);
  }
  if (app_channel_ != nullptr) {
    app_channel_->NotifyState(StateName(to));
  }
  if (options_.observer != nullptr) {
    options_.observer->OnStateChanged(from, to);
  }
  return true;
}

// ---------------------------------------------------------------------------
// 创建与启动

StatusOr<std::unique_ptr<Application>> Application::Create(
    Options options, std::unique_ptr<PlatformIntegration> platform) {
  if (platform == nullptr) {
    return InvalidArgumentError("Application::Create requires a PlatformIntegration");
  }
  if (!platform->native_loop().RunsTasksOnCurrentThread()) {
    return FailedPreconditionError("Application must be created on the OS main thread");
  }
  if (EventLoop::Current() != nullptr) {
    return FailedPreconditionError("The OS main thread already runs an EventLoop");
  }
  // OS 主线程的 EventLoop 跑在原生消息循环之上;绑定后, 主线程上创建的 momo::Object
  // (Application 自身、窗口、VsyncSource 等平台对象)默认归属此循环。
  auto platform_loop =
      EventLoop::Create(std::make_unique<NativeMessagePump>(platform->native_loop()));
  platform_loop->BindToCurrentThread();
  return std::unique_ptr<Application>(
      new Application(std::move(options), std::move(platform), std::move(platform_loop)));
}

Application::Application(Options options, std::unique_ptr<PlatformIntegration> platform,
                         std::shared_ptr<EventLoop> platform_loop)
    : Object(platform_loop->GetTaskRunner()),
      options_(std::move(options)),
      platform_(std::move(platform)),
      platform_loop_(std::move(platform_loop)) {
  thread_host_ = std::make_unique<engine::ThreadHost>(kLabel);
  task_runners_ = thread_host_->MakeTaskRunners(task_runner());
  WindowManager::Observer& observer = *this;  // 私有基类: 在成员上下文内转换
  window_manager_ = std::make_unique<WindowManager>(*platform_, options_.config.window, observer);
}

Application::~Application() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  Terminate(exit_code_);
  // 之后执行的任务(窗口/引擎回调)不再回到正在析构的 Application。
  InvalidateWeakPtrs();
  // 消息循环未运行(启动失败)或提前返回时, 在这里完成 platform EventLoop 的结束语义:
  // 执行剩余已到期任务, 析构所有 DeleteLater 对象。
  platform_loop_->RunUntilIdle();
}

Status Application::Launch() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (!TransitionTo(State::kLaunching)) {
    return FailedPreconditionError("Launch called in state " + std::string(StateName(state_)));
  }

  auto fail = [this](Status status) {
    MOMO_LOG(ERROR) << "Launch failed: " << status;
    Terminate(1);
    return status;
  };

  // 4) 启动窗口(隐式视图 0): 此时只有原生空窗口(闪屏底色), 先显示以尽早给用户反馈。引擎就绪后再绑定。
  if (options_.config.startup_window) {
    if (Status status = window_manager_->OpenStartupWindow(); !status.ok()) {
      return fail(std::move(status));
    }
  }

  // 5) 引擎。
  if (Status status = LaunchEngine(); !status.ok()) {
    return fail(std::move(status));
  }

  TransitionTo(State::kRunning);
  // 启动期间应用可能已被隐藏。
  UpdateSuspended();
  return Status::Ok();
}

Status Application::LaunchEngine() {
  engine::Settings settings;
  settings.dev_mode = options_.config.dev_mode;
  settings.enable_inspector = options_.config.dev_mode && options_.config.inspector_port >= 0;
  settings.inspector_port = options_.config.inspector_port;
  settings.inspector_wait = options_.config.inspector_wait;
  settings.implicit_view = options_.config.startup_window;
  AssetResolver resolver(options_.resources_dir);
  StatusOr<ResolvedAssets> assets = resolver.Resolve(options_.config);
  if (assets.ok()) {
    settings.bundle_path = assets->bundle_path;
    settings.assets_dir = assets->assets_dir;
    settings.extensions_dir = assets->extensions_dir;
  } else if (options_.config.dev_mode) {
    MOMO_LOG(WARNING) << assets.status() << " (--dev: starting without a JS bundle)";
  } else {
    return assets.status();
  }
  // V8 code cache: 用户缓存目录(Mac ~/Library/Caches/<app>/v8, Win %LOCALAPPDATA%\<app>\v8)。
  // --dev 下 bundle 频繁变化, 缓存反复失效, 不启用。
  if (!options_.config.dev_mode) {
#if defined(_WIN32)
    const char* base = std::getenv("LOCALAPPDATA");
    if (base != nullptr) settings.code_cache_dir = std::filesystem::path(base) / "Momo" / "v8";
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      settings.code_cache_dir = std::filesystem::path(home) / "Library" / "Caches" / "dev.momo.app" / "v8";
    }
#endif
  }
  // 主显示器(列表第一个)的刷新率, 供 vsync 回退定时器使用;未知时由引擎取默认值。
  const std::vector<DisplayInfo> displays = platform_->displays();
  if (!displays.empty()) {
    settings.refresh_rate = displays.front().refresh_rate;
  }
  // 宿主通道在 JS bundle 执行之前注册(bundle 顶层代码就可能设置标题、打开窗口、退出)。
  auto register_channels = [this](engine::Shell& shell) {
    AppChannel::Host* app_host = this;  // 私有基类: 在成员上下文内转换
    app_channel_ = std::make_unique<AppChannel>(shell.hub(), task_runner(), app_host);
    window_channel_ = std::make_unique<WindowChannel>(shell.hub(), task_runner(), window_manager_.get());
    platform_channels_ = std::make_unique<PlatformChannels>(
        shell.hub(), task_runner(), &platform_->services(),
        [this] { return window_manager_->FocusedTextInput(); });
  };
  // process.exit(code): JS 已停止, 有序退出。
  auto on_exit_requested = [weak = WeakPtr<Application>(this)](int code) {
    if (Application* self = weak.get()) self->Exit(code);
  };
  StatusOr<std::unique_ptr<engine::Shell>> shell =
      engine::Shell::Create(task_runners_, settings, register_channels, on_exit_requested);
  if (!shell.ok()) {
    app_channel_.reset();
    window_channel_.reset();
    platform_channels_.reset();
    return shell.status();
  }
  shell_ = std::move(shell).value();
  window_manager_->AttachEngine(*shell_, *window_channel_, *platform_channels_);

  // --dev: bundle 变化即 hot restart(docs/04 §8, 先做秒级 hot restart, 不保留 JS 状态)。
  if (options_.config.dev_mode && !settings.bundle_path.empty() &&
      options_.hot_restart_poll > TimeDelta::zero()) {
    std::error_code ec;
    watched_bundle_ = settings.bundle_path;
    watched_bundle_mtime_ = std::filesystem::last_write_time(watched_bundle_, ec);
    task_runner()->PostDelayedTask(
        [weak = WeakPtr<Application>(this)] {
          if (Application* self = weak.get()) self->PollBundleForChanges();
        },
        options_.hot_restart_poll);
  }
  return Status::Ok();
}

void Application::PollBundleForChanges() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shell_ == nullptr || state_ == State::kTerminating) {
    return;
  }
  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(watched_bundle_, ec);
  if (!ec && mtime != watched_bundle_mtime_) {
    watched_bundle_mtime_ = mtime;
    MOMO_LOG(INFO) << "Bundle changed: hot restart.";
    quitting_ = false;  // 旧 JS 的退出流程作废
    ++ask_serial_;
    ask_pending_ = false;
    window_manager_->ResetForHotRestart(options_.config.startup_window);
    shell_->RequestHotRestart();
  }
  task_runner()->PostDelayedTask(
      [weak = WeakPtr<Application>(this)] {
        if (Application* self = weak.get()) self->PollBundleForChanges();
      },
      options_.hot_restart_poll);
}

int Application::Run() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (state_ == State::kRunning || state_ == State::kSuspended) {
    platform_loop_->Run();  // 返回时已排空并清理 DeleteLater 对象
  }
  return exit_code_;
}

// ---------------------------------------------------------------------------
// 应用级事件

void Application::OnAppVisibilityChanged(bool visible) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  app_visible_ = visible;
  UpdateSuspended();
}

void Application::UpdateSuspended() {
  if (!app_visible_ && state_ == State::kRunning) {
    TransitionTo(State::kSuspended);
  } else if (app_visible_ && state_ == State::kSuspended) {
    TransitionTo(State::kRunning);
  }
}

void Application::OnAppActiveChanged(bool active) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (active_ == active) {
    return;
  }
  active_ = active;
  if (app_channel_ != nullptr) {
    app_channel_->NotifyActive(active);
  }
}

void Application::OnReopen(bool has_visible_windows) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (app_channel_ != nullptr && state_ != State::kTerminating) {
    app_channel_->NotifyActivate(has_visible_windows);
  }
}

void Application::OnAllWindowsClosed() {
  if (quitting_ || state_ == State::kTerminating || app_channel_ == nullptr) {
    return;
  }
  MOMO_LOG(INFO) << "The last window was closed; the application keeps running.";
  app_channel_->NotifyWindowAllClosed();
}

// ---------------------------------------------------------------------------
// AppChannel::Host

Status Application::HideApp() { return platform_->app().Hide(); }

Status Application::ShowApp() { return platform_->app().Show(); }

Status Application::FocusApp(bool steal) { return platform_->app().Activate(steal); }

engine::EncodableValue Application::GetAppState() {
  engine::EncodableList caps;
  const uint32_t bits = platform_->app().capabilities();
  if ((bits & kAppCapabilityHide) != 0) caps.emplace_back("hide");
  if ((bits & kAppCapabilityActivate) != 0) caps.emplace_back("activate");
  return engine::EncodableValue(engine::EncodableMap{{"state", std::string(StateName(state_))},
                                                     {"active", active_},
                                                     {"capabilities", std::move(caps)}});
}

// ---------------------------------------------------------------------------
// 退出

uint64_t Application::BeginAsk() {
  const uint64_t serial = ++ask_serial_;
  ask_pending_ = true;
  // JS 长时间不响应(ui 线程卡死, 如死循环)时强制退出: 有序关闭需要与 ui 线程同步汇合, 已不可能。
  if (options_.close_timeout > TimeDelta::zero()) {
    task_runner()->PostDelayedTask(
        [weak = WeakPtr<Application>(this), serial] {
          Application* self = weak.get();
          if (self == nullptr || !self->ask_pending_ || self->ask_serial_ != serial ||
              self->state_ == State::kTerminating) {
            return;  // 已回复
          }
          MOMO_LOG(ERROR) << "The application did not answer the quit request within the timeout "
                             "(UI thread hung?); forcing exit.";
          if (self->options_.force_exit) {
            self->options_.force_exit(1);
          } else {
            std::_Exit(1);
          }
        },
        options_.close_timeout);
  }
  return serial;
}

bool Application::EndAsk(uint64_t serial) {
  if (serial != ask_serial_ || state_ == State::kTerminating) {
    return false;  // 过期的回复(热重启、已退出)
  }
  ask_pending_ = false;
  return true;
}

void Application::Quit() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (state_ == State::kTerminating || quitting_) {
    return;  // 重复请求合并
  }
  if (app_channel_ == nullptr) {  // 引擎尚未就绪: 没有 JS 可询问
    Terminate(exit_code_);
    return;
  }
  quitting_ = true;
  const uint64_t serial = BeginAsk();
  app_channel_->AskBeforeQuit([weak = WeakPtr<Application>(this), serial](bool allow) {
    Application* self = weak.get();
    if (self == nullptr || !self->EndAsk(serial)) return;
    if (!allow) {
      self->AbortQuit("beforeQuit");
      return;
    }
    self->QuitCloseWindows();
  });
}

void Application::QuitCloseWindows() {
  const uint64_t serial = BeginAsk();
  window_manager_->RequestCloseAll([weak = WeakPtr<Application>(this), serial](bool all_closed) {
    Application* self = weak.get();
    if (self == nullptr || !self->EndAsk(serial)) return;
    if (!all_closed) {
      self->AbortQuit("a window refused to close");
      return;
    }
    self->QuitWillQuit();
  });
}

void Application::QuitWillQuit() {
  if (app_channel_ == nullptr) {
    Terminate(exit_code_);
    return;
  }
  const uint64_t serial = BeginAsk();
  app_channel_->AskWillQuit([weak = WeakPtr<Application>(this), serial](bool allow) {
    Application* self = weak.get();
    if (self == nullptr || !self->EndAsk(serial)) return;
    if (!allow) {
      self->AbortQuit("willQuit");
      return;
    }
    self->Terminate(self->exit_code_);
  });
}

void Application::AbortQuit(std::string_view reason) {
  quitting_ = false;
  MOMO_LOG(INFO) << "Quit cancelled by the application (" << reason << ").";
}

void Application::Exit(int exit_code) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (state_ == State::kTerminating) {
    return;
  }
  exit_code_ = exit_code;
  // 可能在宿主通道处理器的调用栈里(Terminate 会移除它): 延后到安全点。
  task_runner()->PostTask([weak = WeakPtr<Application>(this), exit_code] {
    if (Application* self = weak.get()) self->Terminate(exit_code);
  });
}

void Application::Terminate(int exit_code) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (state_ == State::kTerminating) {
    return;
  }
  exit_code_ = exit_code;
  TransitionTo(State::kTerminating);  // 会把 terminating 状态投递给 JS(排在关闭任务之前)

  // 1+2) 窗口先解绑(raster 不再使用它们的 surface)并断开通道 → 移除宿主通道处理器 →
  //      ui: 停帧/卸扩展/销毁 JsRuntime → raster: 释放 GPU 资源。窗口在引擎关闭后再销毁。
  window_manager_->DetachEngine();
  app_channel_.reset();
  window_channel_.reset();
  platform_channels_.reset();
  if (shell_ != nullptr) {
    shell_->Shutdown();
    shell_.reset();
  }
  NotifyShutdownStep(ShutdownStep::kEngineShutdown);

  // 3) 销毁 RenderSurface 与 PlatformWindow(platform 线程)。
  window_manager_->DestroyAll();
  NotifyShutdownStep(ShutdownStep::kWindowDestroyed);

  // 4) join ui → raster → io(顺序由 ThreadHost 保证)。
  thread_host_->Join();
  NotifyShutdownStep(ShutdownStep::kThreadsJoined);

  // 本任务返回后(安全点)EventLoop 排空剩余任务、析构 DeleteLater 对象, 再退出原生循环。
  platform_loop_->Terminate();
  NotifyShutdownStep(ShutdownStep::kMessageLoopQuit);
  MOMO_LOG(INFO) << "Application terminated (exit code " << exit_code_ << ").";
}

void Application::NotifyShutdownStep(ShutdownStep step) {
  if (options_.observer != nullptr) {
    options_.observer->OnShutdownStep(step);
  }
}

}  // namespace momo::embedder
