#include "embedder/common/application.h"

#include <cstdlib>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/native_message_pump.h"
#include "embedder/common/engine_platform.h"
#include "engine/common/app_layout.h"

namespace momo::embedder {

namespace {

constexpr const char kLabel[] = "momo";
// 打包后的应用在 <resources>/app/(docs/09 §4);V8 code cache 在 <cache>/v8/。
constexpr const char kPackagedAppDirName[] = "app";
constexpr const char kCodeCacheDirName[] = "v8";
// 运行时自带的 JS 模块(框架、内置扩展)在 <resources>/builtin/(engine/common/settings.h)。
constexpr const char kBuiltinDirName[] = "builtin";

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
}

Application::~Application() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  Shutdown();
  TearDown();
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
    // 消息循环还没开始: 不会有 Run 来收尾, 在这里销毁已建好的部分。
    Exit(1);
    TearDown();
    return status;
  };

  // 4) 引擎与宿主通道: 只组装, 不等待任何线程, 没有 JS 在运行。
  if (Status status = CreateEngine(); !status.ok()) {
    return fail(std::move(status));
  }
  // 5) 启动窗口(隐式视图 0): 创建即绑定引擎并显示。此时只有闪屏底色, 尽早给用户反馈;bundle 的顶层代码就
  //    可以渲染到它。
  if (options_.config.startup_window) {
    if (Status status = window_manager_->OpenStartupWindow(); !status.ok()) {
      return fail(std::move(status));
    }
  }
  // 6) 启动 JS 运行时: ui 线程执行 bundle, 本线程回到事件循环;结果由 OnRuntimeStarted 接收。
  shell_->Run([weak = WeakPtr<Application>(this)](Status status) {
    if (Application* self = weak.get()) self->OnRuntimeStarted(std::move(status));
  });
  return Status::Ok();
}

void Application::OnRuntimeStarted(Status status) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (state_ != State::kLaunching) {
    return;  // 启动期间已请求退出
  }
  if (!status.ok()) {
    MOMO_LOG(ERROR) << "Launch failed: " << status;
    Exit(1);
    return;
  }
  TransitionTo(State::kRunning);
  // 启动期间应用可能已被隐藏。
  UpdateSuspended();
}

Status Application::CreateEngine() {
  engine::Settings settings;
  settings.dev_mode = options_.config.dev_mode;
  settings.enable_inspector = options_.config.dev_mode && options_.config.inspector_port >= 0;
  settings.inspector_port = options_.config.inspector_port;
  settings.inspector_wait = options_.config.inspector_wait;
  // 应用: 命令行给了路径就用它, 否则是应用包内的 <resources>/app。入口是否存在不在这里检查: 加载失败
  // (不存在、语法错误、顶层抛出)都由引擎在执行入口时报告, 启动随之失败, 进程以非零退出码有序退出。
  MOMO_ASSIGN_OR_RETURN(
      engine::AppLayout app,
      engine::LocateApp(options_.config.app_path.value_or(options_.resources_dir / kPackagedAppDirName)));
  settings.bundle_path = app.entry;
  settings.assets_dir = app.assets_dir;
  settings.builtin_dir = options_.resources_dir / kBuiltinDirName;
  // V8 code cache: 平台入口给出的应用缓存目录下(没给就不缓存)。--dev 下代码频繁变化, 缓存反复失效, 不启用。
  if (!options_.config.dev_mode && !options_.cache_dir.empty()) {
    settings.code_cache_dir = options_.cache_dir / kCodeCacheDirName;
  }
  // 主显示器(列表第一个)的刷新率, 供 vsync 回退定时器使用;未知时由引擎取默认值。
  const std::vector<DisplayInfo> displays = platform_->displays();
  if (!displays.empty()) {
    settings.refresh_rate = displays.front().refresh_rate;
  }
  // process.exit(code): JS 已停止, 有序退出。
  auto on_exit_requested = [weak = WeakPtr<Application>(this)](int code) {
    if (Application* self = weak.get()) self->Exit(code);
  };
  // 引擎: 此时还没有 JS 运行时, 不等待任何线程。
  MOMO_ASSIGN_OR_RETURN(shell_, engine::Shell::Create(task_runners_, settings, on_exit_requested));

  // 宿主通道在 JS bundle 执行之前就位(bundle 顶层代码就可能设置标题、打开窗口、退出)。
  AppChannel::Host* app_host = this;  // 私有基类: 在成员上下文内转换
  app_channel_ = std::make_unique<AppChannel>(shell_->hub(), task_runner(), app_host);
  // 输入法会话在获得焦点的窗口上(窗口注册表稍后创建, 调用时已存在)。
  text_input_channel_ = std::make_unique<TextInputChannel>(
      shell_->hub(), task_runner(), [this] { return window_manager_->FocusedTextInput(); });
  platform_channels_ = std::make_unique<PlatformChannels>(shell_->hub(), task_runner(), platform_->services());
  MenuChannel::Host* menu_host = this;
  menu_channel_ = std::make_unique<MenuChannel>(shell_->hub(), task_runner(), platform_->menus(), menu_host);
  tray_channel_ = std::make_unique<TrayChannel>(shell_->hub(), task_runner(), platform_->menus());
  WindowManager::Observer& observer = *this;
  window_manager_ = std::make_unique<WindowManager>(*platform_, options_.config.window, observer, *shell_,
                                                    *text_input_channel_);
  return Status::Ok();
}

int Application::Run() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  // Launching 时也进入循环: JS 运行时的启动结果要在这里到达。
  if (state_ == State::kLaunching || state_ == State::kRunning || state_ == State::kSuspended) {
    platform_loop_->Run();  // 返回时已排空并清理 DeleteLater 对象
  }
  TearDown();
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
// MenuChannel::Host

PlatformWindow* Application::FindWindow(engine::platform::ViewId view) { return window_manager_->FindWindow(view); }

void Application::PerformMenuRole(MenuRole role) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  auto edit = [this](const char* command) {
    // 与输入法的编辑命令同路: 交给获得文本焦点的 EditableText。
    text_input_channel_->Send({.type = TextInputEvent::Type::kCommand, .command = command});
  };
  WindowController* focused = window_manager_->focused_window();
  switch (role) {
    case MenuRole::kQuit: Quit(); return;
    case MenuRole::kCut: edit("cut"); return;
    case MenuRole::kCopy: edit("copy"); return;
    case MenuRole::kPaste: edit("paste"); return;
    case MenuRole::kSelectAll: edit("selectAll"); return;
    case MenuRole::kClose:
      if (focused != nullptr) window_manager_->RequestCloseWindow(focused->view_id());
      return;
    case MenuRole::kMinimize:
      if (focused != nullptr) (void)focused->window().Minimize();
      return;
    case MenuRole::kZoom:
      if (focused != nullptr) {
        (void)(focused->window().state().maximized ? focused->window().Restore() : focused->window().Maximize());
      }
      return;
    default:
      return;  // 其余 role 只存在于有系统标准动作的平台(macOS), 由系统执行
  }
}

// ---------------------------------------------------------------------------
// 退出

void Application::Quit() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (state_ == State::kTerminating || quitting_) {
    return;  // 重复请求合并
  }
  quitting_ = true;
  if (app_channel_ == nullptr) {  // 尚未 Launch: 没有 JS 可询问, 也没有窗口
    Shutdown();
    return;
  }
  // 启动期间(bundle 还在执行)也照常询问: 消息排在 bundle 之后;JS 没有注册处理器时答复为"允许"。
  app_channel_->AskBeforeQuit([weak = WeakPtr<Application>(this)](bool allow) {
    Application* self = weak.get();
    if (self == nullptr || !self->quitting_) return;  // 期间已经 Exit
    if (!allow) {
      self->AbortQuit("beforeQuit");
      return;
    }
    self->QuitCloseWindows();
  });
}

void Application::QuitCloseWindows() {
  window_manager_->RequestCloseAll([weak = WeakPtr<Application>(this)](bool all_closed) {
    Application* self = weak.get();
    if (self == nullptr || !self->quitting_) return;
    if (!all_closed) {
      self->AbortQuit("a window refused to close");
      return;
    }
    self->QuitWillQuit();
  });
}

void Application::QuitWillQuit() {
  app_channel_->AskWillQuit([weak = WeakPtr<Application>(this)](bool allow) {
    Application* self = weak.get();
    if (self == nullptr || !self->quitting_) return;
    if (!allow) {
      self->AbortQuit("willQuit");
      return;
    }
    self->Shutdown();
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
  // 不询问, 但窗口照常关闭(JS 收到各自的 'closed'): 退出时不留下还开着的窗口(同 Electron app.exit)。
  if (window_manager_ != nullptr) {
    window_manager_->CloseAll();
  }
  Shutdown();
}

void Application::Shutdown() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (state_ == State::kTerminating) {
    return;
  }
  quitting_ = false;
  TransitionTo(State::kTerminating);  // JS 收到 terminating(排在销毁之前, 照常执行)
  // 打开着的上下文菜单在系统的菜单跟踪循环里: 不关掉它, 消息循环要等用户关闭菜单才结束。
  if (PlatformMenus* menus = platform_->menus()) {
    menus->ClosePopup();
  }
  // 本任务返回后(安全点)EventLoop 排空剩余任务、析构 DeleteLater 对象, 再退出原生循环;销毁在循环结束之后。
  platform_loop_->Terminate();
  NotifyShutdownStep(ShutdownStep::kMessageLoopQuit);
}

void Application::TearDown() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (torn_down_) {
    return;
  }
  torn_down_ = true;

  if (shell_ != nullptr) {  // 已 Launch
    // 1+2) 引擎最先(Shell::Shutdown): 平台视图(移出窗口后交给扩展释放)→ ui 线程销毁 JS 运行时(此前投递给它的
    //      事件都已执行完)→ raster 上的 GPU 资源(同步停用各窗口的 surface)。
    //      窗口此时仍绑定着: 平台视图要从它们身上移除。与 ui 线程汇合: JS 不返回就在这里等它。
    shell_->Shutdown();
    // 已没有 JS, 也没有人使用 surface: 窗口放开引擎, 移除宿主通道。
    window_manager_->DetachEngine();
    app_channel_.reset();
    text_input_channel_.reset();
    platform_channels_.reset();
    menu_channel_.reset();
    tray_channel_.reset();  // 移除托盘图标
    NotifyShutdownStep(ShutdownStep::kEngineShutdown);

    // 3) 销毁 RenderSurface 与 PlatformWindow(platform 线程), 连同 momo/window;最后是 Shell。
    window_manager_.reset();
    shell_.reset();
    NotifyShutdownStep(ShutdownStep::kWindowDestroyed);
  }

  // 4) join ui → raster → io(顺序由 ThreadHost 保证)。
  thread_host_->Join();
  NotifyShutdownStep(ShutdownStep::kThreadsJoined);
  MOMO_LOG(INFO) << "Application terminated (exit code " << exit_code_ << ").";
}

void Application::NotifyShutdownStep(ShutdownStep step) {
  if (options_.observer != nullptr) {
    options_.observer->OnShutdownStep(step);
  }
}

}  // namespace momo::embedder
