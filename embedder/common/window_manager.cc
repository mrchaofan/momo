#include "embedder/common/window_manager.h"

#include <utility>

#include "base/logging.h"

namespace momo::embedder {

using engine::platform::kImplicitViewId;
using engine::platform::ViewId;

WindowManager::WindowManager(PlatformIntegration& platform, WindowSpec startup_spec, Observer& observer)
    : platform_(platform), startup_spec_(std::move(startup_spec)), observer_(observer) {}

WindowManager::~WindowManager() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  DetachEngine();
  DestroyAll();
}

// ---------------------------------------------------------------------------
// 创建与绑定

StatusOr<std::unique_ptr<WindowController>> WindowManager::NewWindow(ViewId view_id, const WindowSpec& spec) {
  WindowController::Client& client = *this;  // 私有基类: 在成员上下文内转换
  auto controller = std::make_unique<WindowController>(client, view_id);
  MOMO_RETURN_IF_ERROR(controller->CreateWindow(platform_, spec));
  return controller;
}

Status WindowManager::OpenStartupWindow() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_ASSIGN_OR_RETURN(std::unique_ptr<WindowController> controller,
                        NewWindow(kImplicitViewId, startup_spec_));
  Open(std::move(controller), /*show=*/true);
  return Status::Ok();
}

void WindowManager::Open(std::unique_ptr<WindowController> controller, bool show) {
  const ViewId view_id = controller->view_id();
  WindowController& raw = *controller;
  windows_[view_id] = std::move(controller);
  if (show) {
    raw.window().Show();
  }
  if (shell_ != nullptr) {
    Attach(raw);
  }
  MOMO_LOG(INFO) << "Window opened (view " << view_id << (show ? "" : ", hidden") << ").";
}

void WindowManager::Attach(WindowController& controller) {
  StatusOr<ObjectPtr<platform::VsyncSource>> vsync = platform_.CreateVsyncSource(controller.window());
  if (!vsync.ok()) {
    MOMO_LOG(VERBOSE) << "No platform vsync for view " << controller.view_id() << " (" << vsync.status()
                      << "); the engine falls back to a timer.";
  }
  controller.AttachEngine(*shell_, vsync.ok() ? std::move(vsync).value() : nullptr);
}

void WindowManager::AttachEngine(engine::Shell& shell, WindowChannel& window_channel,
                                 PlatformChannels& platform_channels) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_DCHECK(shell_ == nullptr);
  shell_ = &shell;
  window_channel_ = &window_channel;
  platform_channels_ = &platform_channels;
  for (auto& [view_id, controller] : windows_) {
    Attach(*controller);
  }
}

void WindowManager::DetachEngine() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  for (auto& [view_id, controller] : windows_) {
    controller->DetachEngine();
  }
  shell_ = nullptr;
  window_channel_ = nullptr;
  platform_channels_ = nullptr;
}

void WindowManager::DestroyAll() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_DCHECK(shell_ == nullptr) << "DetachEngine must precede DestroyAll";
  windows_.clear();
  focused_window_.reset();
}

StatusOr<ViewId> WindowManager::CreateWindow(const WindowOptions& options) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shell_ == nullptr) {
    return FailedPreconditionError("the engine is not running");
  }
  WindowSpec spec = startup_spec_;  // 继承启动窗口的闪屏底色等
  spec.title = options.title.empty() ? spec.title : options.title;
  spec.size = options.size;
  spec.min_size = options.min_size;
  spec.position = options.position;
  spec.non_activating = options.non_activating;
  spec.resizable = options.resizable;
  const ViewId view_id = next_view_id_++;
  MOMO_ASSIGN_OR_RETURN(std::unique_ptr<WindowController> controller, NewWindow(view_id, spec));
  Open(std::move(controller), options.show);
  return view_id;
}

// ---------------------------------------------------------------------------
// 关闭

void WindowManager::CloseWindow(ViewId view_id) { Close(view_id, /*report_all_closed=*/true); }

void WindowManager::Close(ViewId view_id, bool report_all_closed) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  auto it = windows_.find(view_id);
  if (it == windows_.end()) {
    return;
  }
  std::unique_ptr<WindowController> controller = std::move(it->second);
  windows_.erase(it);
  // 进行中的指针序列若由这个窗口接收: 先以取消结束它(此时引擎仍绑定, 框架收得到), 再解绑。
  // 序列已改交别的窗口时(拖动中并入另一个窗口)什么也不做, 拖动继续。
  if (controller->has_window()) {
    controller->window().ReleasePointerCapture();
  }
  // 先解绑(raster 同步停用 surface, JS 收到 onViewportsChanged), 再销毁窗口。
  controller->DetachEngine();
  if (window_channel_ != nullptr) {
    window_channel_->NotifyClosed(view_id);
  }
  if (focused_window_ == view_id) {
    focused_window_.reset();
  }
  // 关闭按钮的回调可能仍在窗口自身的调用栈上: 延后到安全点销毁。用 DeleteLater 而不是普通任务: 普通任务也会在
  // 嵌套循环里执行(拖拽缩放时等待新帧的 RunDueTasksNested、系统模态循环), 那时窗口的方法可能还在栈上;
  // DeleteLater 推迟到栈上没有任务的最外层安全点。
  controller.release()->DeleteLater();
  MOMO_LOG(INFO) << "Window closed (view " << view_id << ").";
  // 最后一个窗口: 延后通知——当前可能在宿主通道处理器的调用栈里;期间又打开了窗口就不通知。
  if (windows_.empty() && report_all_closed) {
    task_runner()->PostTask([weak = WeakPtr<WindowManager>(this)] {
      WindowManager* self = weak.get();
      if (self != nullptr && self->windows_.empty()) {
        self->observer_.OnAllWindowsClosed();
      }
    });
  }
}

void WindowManager::RequestCloseWindow(ViewId view_id) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (windows_.count(view_id) == 0) {
    return;
  }
  if (window_channel_ == nullptr) {  // 引擎尚未就绪(启动中)或已关闭: 直接关闭
    CloseWindow(view_id);
    return;
  }
  window_channel_->RequestClose(view_id, [weak = WeakPtr<WindowManager>(this), view_id](bool allow) {
    WindowManager* self = weak.get();
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

void WindowManager::RequestCloseAll(MoveOnlyFunction<void(bool all_closed)> done) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  const std::vector<ViewId> ids = view_ids();
  if (ids.empty() || window_channel_ == nullptr) {
    for (ViewId id : ids) {
      Close(id, /*report_all_closed=*/false);
    }
    task_runner()->PostTask([done = std::move(done)]() mutable { done(true); });
    return;
  }
  struct Round {
    size_t pending;
    bool refused = false;
    MoveOnlyFunction<void(bool)> done;
  };
  auto round = std::make_shared<Round>(Round{ids.size(), false, std::move(done)});
  WeakPtr<WindowManager> weak(this);
  for (ViewId id : ids) {
    window_channel_->RequestClose(id, [weak, round, id](bool allow) {
      WindowManager* self = weak.get();
      if (allow && self != nullptr) {
        self->Close(id, /*report_all_closed=*/false);
      } else if (!allow) {
        round->refused = true;
        MOMO_LOG(INFO) << "Closing view " << id << " was cancelled by the application.";
      }
      if (--round->pending == 0) {
        round->done(!round->refused && (self == nullptr || self->windows_.empty()));
      }
    });
  }
}

void WindowManager::ResetForHotRestart(bool keep_startup_window) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (keep_startup_window && windows_.count(kImplicitViewId) == 0) {
    StatusOr<std::unique_ptr<WindowController>> startup = NewWindow(kImplicitViewId, startup_spec_);
    if (startup.ok()) {
      Open(std::move(startup).value(), /*show=*/true);
    } else {
      MOMO_LOG(ERROR) << "Hot restart: cannot reopen the startup window: " << startup.status();
    }
  }
  for (ViewId id : view_ids()) {
    if (!keep_startup_window || id != kImplicitViewId) {
      Close(id, /*report_all_closed=*/false);
    }
  }
}

// ---------------------------------------------------------------------------
// 查询

bool WindowManager::has_visible_window() const {
  for (const auto& [view_id, controller] : windows_) {
    if (controller->state().shown()) return true;
  }
  return false;
}

WindowController* WindowManager::window(ViewId view_id) const {
  auto it = windows_.find(view_id);
  return it != windows_.end() ? it->second.get() : nullptr;
}

WindowController* WindowManager::front_window() const {
  return windows_.empty() ? nullptr : windows_.begin()->second.get();  // map 按 id 升序: 视图 0 在前
}

std::vector<ViewId> WindowManager::view_ids() const {
  std::vector<ViewId> ids;
  ids.reserve(windows_.size());
  for (const auto& [view_id, controller] : windows_) {
    ids.push_back(view_id);
  }
  return ids;
}

PlatformWindow* WindowManager::FindWindow(ViewId view_id) {
  WindowController* controller = window(view_id);
  return controller != nullptr && controller->has_window() ? &controller->window() : nullptr;
}

TextInputConnection* WindowManager::FocusedTextInput() {
  WindowController* controller = focused_window_.has_value() ? window(*focused_window_) : nullptr;
  if (controller == nullptr) {
    controller = front_window();
  }
  return controller != nullptr && controller->has_window() ? &controller->window().text_input() : nullptr;
}

// ---------------------------------------------------------------------------
// WindowController::Client

void WindowManager::OnWindowCloseRequested(ViewId view) { RequestCloseWindow(view); }

void WindowManager::OnWindowStateChanged(ViewId view, const WindowStateFlags& state) {
  if (window_channel_ != nullptr) window_channel_->NotifyStateChanged(view, state);
}

void WindowManager::OnWindowFocusChanged(ViewId view, bool focused) {
  if (window_channel_ != nullptr) window_channel_->NotifyFocusChanged(view, focused);
  if (focused) {
    focused_window_ = view;
  } else if (focused_window_ == view) {
    focused_window_.reset();
  }
}

void WindowManager::OnWindowTextInput(ViewId, const TextInputEvent& event) {
  if (platform_channels_ != nullptr) platform_channels_->SendTextInput(event);
}

void WindowManager::OnWindowFrameChanged(ViewId view, const Rect& screen_frame) {
  if (window_channel_ != nullptr) window_channel_->NotifyFrameChanged(view, screen_frame);
}

void WindowManager::OnWindowControlsChanged(ViewId view, const std::optional<Rect>& controls) {
  if (window_channel_ != nullptr) window_channel_->NotifyControlsChanged(view, controls);
}

void WindowManager::OnWindowFirstFrame(ViewId view) {
  if (window_channel_ != nullptr) window_channel_->NotifyReadyToShow(view);
}

}  // namespace momo::embedder
