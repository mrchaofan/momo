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
    case State::kNotRunning: return "NotRunning";
    case State::kLaunching: return "Launching";
    case State::kRunning: return "Running";
    case State::kSuspended: return "Suspended";
    case State::kTerminating: return "Terminating";
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
    switch (to) {
      case State::kRunning:
        shell_->NotifyLifecycleEvent(engine::LifecycleEvent::kResumed);
        break;
      case State::kSuspended:
        shell_->NotifyLifecycleEvent(engine::LifecycleEvent::kSuspended);
        break;
      case State::kTerminating:
        shell_->NotifyLifecycleEvent(engine::LifecycleEvent::kTerminating);
        break;
      default:
        break;
    }
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

  // 4) 隐式视图 0 的窗口: 此时只有原生空窗口(闪屏底色), 先显示以尽早给用户反馈。
  //    引擎就绪后再绑定(LaunchEngine)。
  StatusOr<std::unique_ptr<WindowController>> first =
      NewWindow(engine::platform::kImplicitViewId, options_.config.window);
  if (!first.ok()) {
    return fail(first.status());
  }
  (*first)->window().Show();
  windows_[engine::platform::kImplicitViewId] = std::move(first).value();

  // 5) 引擎。
  if (Status status = LaunchEngine(); !status.ok()) {
    return fail(std::move(status));
  }

  TransitionTo(State::kRunning);
  // 启动期间窗口可能已被最小化/应用被隐藏。
  UpdateSuspended();
  return Status::Ok();
}

Status Application::LaunchEngine() {
  engine::Settings settings;
  settings.dev_mode = options_.config.dev_mode;
  settings.enable_inspector = options_.config.dev_mode && options_.config.inspector_port >= 0;
  settings.inspector_port = options_.config.inspector_port;
  settings.inspector_wait = options_.config.inspector_wait;
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
  // 宿主通道在 JS bundle 执行之前注册(bundle 顶层代码就可能设置标题、打开窗口)。
  auto register_channels = [this](engine::Shell& shell) {
    WindowChannel::Host* host = this;  // 私有基类: 在成员上下文内转换
    window_channel_ = std::make_unique<WindowChannel>(shell.hub(), task_runner(), host);
    platform_channels_ = std::make_unique<PlatformChannels>(
        shell.hub(), task_runner(), &platform_->services(), [this] { return FocusedTextInput(); });
  };
  StatusOr<std::unique_ptr<engine::Shell>> shell =
      engine::Shell::Create(task_runners_, settings, register_channels);
  if (!shell.ok()) {
    window_channel_.reset();
    platform_channels_.reset();
    return shell.status();
  }
  shell_ = std::move(shell).value();

  // 隐式视图 0 的窗口绑定引擎(VsyncSource 不可用时 Shell 回退定时器)。
  auto it = windows_.find(engine::platform::kImplicitViewId);
  if (it != windows_.end()) {
    WindowController& first = *it->second;
    StatusOr<ObjectPtr<platform::VsyncSource>> vsync = platform_->CreateVsyncSource(first.window());
    if (!vsync.ok()) {
      MOMO_LOG(VERBOSE) << "No platform vsync (" << vsync.status() << "); falling back to a timer.";
    }
    first.AttachEngine(*shell_, vsync.ok() ? std::move(vsync).value() : nullptr);
  }

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
    // 新 bundle 从隐式视图开始: 保证它存在(之前被关闭则重新打开), 再关闭旧 JS 打开的其他窗口。
    if (windows_.count(engine::platform::kImplicitViewId) == 0) {
      StatusOr<std::unique_ptr<WindowController>> first =
          NewWindow(engine::platform::kImplicitViewId, options_.config.window);
      if (first.ok()) {
        OpenWindow(std::move(first).value());
      } else {
        MOMO_LOG(ERROR) << "Hot restart: cannot reopen the startup window: " << first.status();
      }
    }
    std::vector<engine::platform::ViewId> others;
    for (const auto& [view_id, controller] : windows_) {
      if (view_id != engine::platform::kImplicitViewId) others.push_back(view_id);
    }
    for (engine::platform::ViewId view_id : others) {
      CloseWindow(view_id);
    }
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
// 运行期

void Application::OnAppVisibilityChanged(bool visible) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  app_visible_ = visible;
  UpdateSuspended();
}

void Application::OnWindowVisibilityChanged(engine::platform::ViewId view_id, bool visible) {
  if (visible) {
    minimized_windows_.erase(view_id);
  } else {
    minimized_windows_.insert(view_id);
  }
  UpdateSuspended();
}

void Application::UpdateSuspended() {
  // 所有窗口都最小化才算不可见(还有任一窗口可见就继续出帧)。
  const bool all_minimized = minimized_windows_.size() >= windows_.size();
  const bool should_suspend = !app_visible_ || all_minimized;
  if (should_suspend && state_ == State::kRunning) {
    TransitionTo(State::kSuspended);
  } else if (!should_suspend && state_ == State::kSuspended) {
    TransitionTo(State::kRunning);
  }
}

// ---------------------------------------------------------------------------
// 退出与有序关闭

void Application::RequestTerminate() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (state_ == State::kTerminating || terminate_requested_) {
    return;
  }
  if (shell_ == nullptr || window_channel_ == nullptr) {
    Terminate(exit_code_);
    return;
  }
  terminate_requested_ = true;
  const uint64_t serial = ++close_request_serial_;
  window_channel_->RequestAppExit([weak = WeakPtr<Application>(this)](bool allow) {
    Application* self = weak.get();
    if (self == nullptr) {
      return;
    }
    self->terminate_requested_ = false;
    if (allow) {
      self->Terminate(self->exit_code_);
    } else {
      MOMO_LOG(INFO) << "Close request cancelled by the application.";
    }
  });
  // JS 长时间不响应(ui 线程卡死, 如死循环)时强制退出: 有序关闭需要与 ui 线程同步汇合, 已不可能。
  if (options_.close_timeout > TimeDelta::zero()) {
    task_runner()->PostDelayedTask(
        [weak = WeakPtr<Application>(this), serial] {
          Application* self = weak.get();
          if (self == nullptr || !self->terminate_requested_ ||
              self->close_request_serial_ != serial || self->state_ == State::kTerminating) {
            return;  // 已回复(放行或取消)
          }
          MOMO_LOG(ERROR) << "The application did not answer the close request within the "
                             "timeout (UI thread hung?); forcing exit.";
          if (self->options_.force_exit) {
            self->options_.force_exit(1);
          } else {
            std::_Exit(1);
          }
        },
        options_.close_timeout);
  }
}

void Application::Terminate(int exit_code) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (state_ == State::kTerminating) {
    return;
  }
  exit_code_ = exit_code;
  TransitionTo(State::kTerminating);  // 会把 terminating 事件投递给 JS(排在关闭任务之前)

  // 1+2) ui: 停帧/卸扩展/销毁 JsRuntime → raster: 释放 GPU 资源并解绑 surface。
  //      先移除宿主通道处理器;窗口先解绑(raster 不再使用它们的 surface), 引擎关闭后再销毁。
  window_channel_.reset();
  platform_channels_.reset();
  if (shell_ != nullptr) {
    for (auto& [view_id, controller] : windows_) {
      controller->DetachEngine();
    }
    shell_->Shutdown();
    shell_.reset();
  }
  NotifyShutdownStep(ShutdownStep::kEngineShutdown);

  // 3) 销毁 RenderSurface 与 PlatformWindow(platform 线程)。
  windows_.clear();
  NotifyShutdownStep(ShutdownStep::kWindowDestroyed);

  // 4) join ui → raster → io(顺序由 ThreadHost 保证)。
  thread_host_->Join();
  NotifyShutdownStep(ShutdownStep::kThreadsJoined);

  // 本任务返回后(安全点)EventLoop 排空剩余任务、析构 DeleteLater 对象, 再退出原生循环。
  platform_loop_->Terminate();
  NotifyShutdownStep(ShutdownStep::kMessageLoopQuit);
  MOMO_LOG(INFO) << "Application terminated (exit code " << exit_code_ << ").";
}

// ---------------------------------------------------------------------------
// 窗口(WindowChannel::Host)

WindowController* Application::window(engine::platform::ViewId view_id) const {
  auto it = windows_.find(view_id);
  return it != windows_.end() ? it->second.get() : nullptr;
}

WindowController* Application::front_window() const {
  return windows_.empty() ? nullptr : windows_.begin()->second.get();  // map 按 id 升序: 视图 0 在前
}

PlatformWindow* Application::FindWindow(engine::platform::ViewId view_id) {
  WindowController* controller = window(view_id);
  return controller != nullptr && controller->has_window() ? &controller->window() : nullptr;
}

StatusOr<std::unique_ptr<WindowController>> Application::NewWindow(engine::platform::ViewId view_id,
                                                                   const WindowSpec& spec) {
  WeakPtr<Application> weak(this);
  WindowController::Callbacks callbacks;
  callbacks.on_close_requested = [weak, view_id] {
    if (Application* self = weak.get()) self->RequestCloseWindow(view_id);
  };
  callbacks.on_visibility_changed = [weak, view_id](bool visible) {
    if (Application* self = weak.get()) self->OnWindowVisibilityChanged(view_id, visible);
  };
  callbacks.on_focus_changed = [weak, view_id](bool focused) {
    if (Application* self = weak.get()) self->OnWindowFocusChanged(view_id, focused);
  };
  callbacks.on_text_input = [weak](const TextInputEvent& event) {
    Application* self = weak.get();
    if (self != nullptr && self->platform_channels_ != nullptr) self->platform_channels_->SendTextInput(event);
  };
  callbacks.on_frame_changed = [weak, view_id](const Rect& frame) {
    Application* self = weak.get();
    if (self != nullptr && self->window_channel_ != nullptr) self->window_channel_->NotifyFrameChanged(view_id, frame);
  };
  callbacks.on_controls_changed = [weak, view_id](const std::optional<Rect>& controls) {
    Application* self = weak.get();
    if (self != nullptr && self->window_channel_ != nullptr) {
      self->window_channel_->NotifyControlsChanged(view_id, controls);
    }
  };
  auto controller = std::make_unique<WindowController>(std::move(callbacks), view_id);
  MOMO_RETURN_IF_ERROR(controller->CreateWindow(*platform_, spec));
  return controller;
}

void Application::OpenWindow(std::unique_ptr<WindowController> controller) {
  MOMO_DCHECK(shell_ != nullptr);
  const engine::platform::ViewId view_id = controller->view_id();
  controller->window().Show();
  StatusOr<ObjectPtr<platform::VsyncSource>> vsync = platform_->CreateVsyncSource(controller->window());
  controller->AttachEngine(*shell_, vsync.ok() ? std::move(vsync).value() : nullptr);
  windows_[view_id] = std::move(controller);
  MOMO_LOG(INFO) << "Window opened (view " << view_id << ").";
  UpdateSuspended();
}

StatusOr<engine::platform::ViewId> Application::CreateWindow(const WindowOptions& options) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shell_ == nullptr || state_ == State::kTerminating) {
    return FailedPreconditionError("the application is shutting down");
  }
  WindowSpec spec = options_.config.window;  // 继承启动窗口的闪屏底色等
  spec.title = options.title.empty() ? spec.title : options.title;
  spec.size = options.size;
  spec.min_size = options.min_size;
  spec.position = options.position;
  spec.non_activating = options.non_activating;
  spec.resizable = options.resizable;
  const engine::platform::ViewId view_id = next_view_id_++;
  MOMO_ASSIGN_OR_RETURN(std::unique_ptr<WindowController> controller, NewWindow(view_id, spec));
  OpenWindow(std::move(controller));
  return view_id;
}

void Application::CloseWindow(engine::platform::ViewId view_id) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  auto it = windows_.find(view_id);
  if (it == windows_.end()) {
    return;
  }
  // 先解绑(raster 同步停用 surface, JS 收到 onViewsChanged), 再销毁窗口。
  std::unique_ptr<WindowController> controller = std::move(it->second);
  windows_.erase(it);
  // 进行中的指针序列若由这个窗口接收: 先以取消结束它(此时引擎仍绑定, 框架收得到), 再解绑。
  // 序列已改交别的窗口时(拖动中并入另一个窗口)什么也不做, 拖动继续。
  if (controller->has_window()) {
    controller->window().ReleasePointerCapture();
  }
  controller->DetachEngine();
  minimized_windows_.erase(view_id);
  if (focused_window_ == view_id) {
    focused_window_.reset();
  }
  // 关闭按钮的回调可能仍在窗口自身的调用栈上: 延后到安全点销毁。
  task_runner()->PostTask([controller = std::move(controller)]() mutable { controller.reset(); });
  MOMO_LOG(INFO) << "Window closed (view " << view_id << ").";
  UpdateSuspended();
  // 最后一个窗口关闭: 退出应用。延后执行——当前可能在宿主通道处理器的调用栈里, 而 Terminate 会移除它;
  // 期间若又打开了窗口(热重启、JS 立刻新开)就不退出。
  if (windows_.empty()) {
    task_runner()->PostTask([weak = WeakPtr<Application>(this)] {
      Application* self = weak.get();
      if (self != nullptr && self->windows_.empty() && self->state_ != State::kTerminating) {
        MOMO_LOG(INFO) << "The last window was closed: exiting.";
        self->Terminate(self->exit_code_);
      }
    });
  }
}

void Application::RequestCloseWindow(engine::platform::ViewId view_id) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (windows_.count(view_id) == 0 || state_ == State::kTerminating) {
    return;
  }
  if (window_channel_ == nullptr) {  // 引擎尚未就绪(启动中): 直接关闭
    CloseWindow(view_id);
    return;
  }
  window_channel_->RequestClose(view_id, [weak = WeakPtr<Application>(this), view_id](bool allow) {
    Application* self = weak.get();
    if (self == nullptr) {
      return;
    }
    if (allow) {
      self->CloseWindow(view_id);
    } else {
      MOMO_LOG(INFO) << "Closing view " << view_id << " was cancelled by the application.";
    }
  });
}

void Application::OnWindowFocusChanged(engine::platform::ViewId view_id, bool focused) {
  if (focused) {
    focused_window_ = view_id;
  } else if (focused_window_ == view_id) {
    focused_window_.reset();
  }
}

TextInputConnection* Application::FocusedTextInput() {
  WindowController* controller = focused_window_.has_value() ? window(*focused_window_) : nullptr;
  if (controller == nullptr && !windows_.empty()) {
    controller = windows_.begin()->second.get();
  }
  return controller != nullptr && controller->has_window() ? &controller->window().text_input()
                                                           : nullptr;
}

void Application::NotifyShutdownStep(ShutdownStep step) {
  if (options_.observer != nullptr) {
    options_.observer->OnShutdownStep(step);
  }
}

}  // namespace momo::embedder
