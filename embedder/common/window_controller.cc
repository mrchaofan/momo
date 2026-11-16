#include "embedder/common/window_controller.h"

#include <utility>

#include "base/logging.h"

namespace momo::embedder {

StatusOr<std::unique_ptr<WindowController>> WindowController::Create(PlatformIntegration& platform,
                                                                     const WindowOptions& options,
                                                                     Client& client,
                                                                     engine::platform::ViewId view_id) {
  std::unique_ptr<WindowController> controller(new WindowController(client, view_id));
  MOMO_ASSIGN_OR_RETURN(controller->window_, platform.CreateWindow(options, controller.get()));
  // 以创建完成后的窗口为准(创建过程中的回调可能已经写过缓存)。
  PlatformWindow& window = *controller->window_;
  controller->metrics_ = window.metrics();
  controller->state_ = window.state();
  controller->frame_.reset();
  controller->controls_ = window.window_controls_rect();
  return controller;
}

WindowController::WindowController(Client& client, engine::platform::ViewId view_id)
    : client_(client), view_id_(view_id) {}

WindowController::~WindowController() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_DCHECK(shell_ == nullptr) << "Window destroyed while the engine still renders into it";
  window_.reset();  // 销毁过程中的窗口回调仍落在完整的对象上
}

void WindowController::AttachEngine(engine::Shell& shell, WindowChannel& events) {
  MOMO_DCHECK(shell_ == nullptr);
  shell_ = &shell;
  events_ = &events;
  shell.AttachView(view_id_, this, &window_->surface());
  shell.OnMetricsChanged(view_id_, metrics_);
  rendering_ = ShouldRender();
  if (!rendering_) {
    shell.OnViewVisibilityChanged(view_id_, false);
  }
}

bool WindowController::ShouldRender() const {
  // 以 show:false 创建、尚未显示过的窗口也出帧: 第一帧呈现后才发 readyToShow(应用据此显示)。
  return state_.shown() || (!first_frame_ && !state_.minimized);
}

void WindowController::UpdateRendering() {
  const bool rendering = ShouldRender();
  if (rendering == rendering_) {
    return;
  }
  rendering_ = rendering;
  if (shell_ != nullptr) {
    shell_->OnViewVisibilityChanged(view_id_, rendering);
  }
}

void WindowController::DetachEngine() {
  if (shell_ == nullptr) {
    return;
  }
  shell_->DetachView(view_id_);
  shell_ = nullptr;
  events_ = nullptr;
}

void WindowController::OnMetricsChanged(const platform::ViewportMetrics& metrics) {
  if (metrics == metrics_) {
    return;
  }
  metrics_ = metrics;
  if (shell_ != nullptr) {
    shell_->OnMetricsChanged(view_id_, metrics);
  }
}

void WindowController::OnPointerPacket(platform::PointerDataPacket packet) {
  if (shell_ != nullptr) {
    shell_->OnPointerPacket(view_id_, std::move(packet));
  }
}

void WindowController::OnKeyData(const platform::KeyData& data) {
  if (shell_ != nullptr) {
    shell_->OnKeyData(view_id_, data);
  }
}

bool WindowController::OnCloseRequested() {
  // 总是先拦截: 关闭需要询问 JS(异步, ui 线程), 且销毁必须按 docs/01 §4 的顺序进行。
  client_.OnWindowCloseRequested(view_id_);
  return false;
}

void WindowController::OnFocusChanged(bool focused) {
  if (focused == focused_) {
    return;
  }
  focused_ = focused;
  if (events_ != nullptr) {
    events_->NotifyFocusChanged(view_id_, focused);
  }
}

void WindowController::OnWindowStateChanged(const WindowStateFlags& state) {
  if (state == state_) {
    return;
  }
  state_ = state;
  UpdateRendering();
  if (events_ != nullptr) {
    events_->NotifyStateChanged(view_id_, state);
  }
}

void WindowController::OnTextInput(const TextInputEvent& event) {
  client_.OnWindowTextInput(view_id_, event);
}

void WindowController::OnPlatformViewFocused(int64_t view_id) {
  if (shell_ != nullptr) {
    shell_->OnPlatformViewFocused(view_id);
  }
}

void WindowController::OnPlatformViewCreated(int64_t view_id, void* native_view) {
  window_->AddPlatformView(view_id, native_view);
}

void WindowController::OnPlatformViewDisposed(int64_t view_id) { window_->RemovePlatformView(view_id); }

void WindowController::OnPlatformViewFocusRequested(int64_t view_id) { window_->FocusPlatformView(view_id); }

void WindowController::OnPlatformViewBlurRequested(int64_t view_id) { window_->BlurPlatformView(view_id); }


void WindowController::OnFirstFramePresented() {
  if (first_frame_) {
    return;
  }
  first_frame_ = true;
  UpdateRendering();
  if (events_ != nullptr) {
    events_->NotifyReadyToShow(view_id_);
  }
}

void WindowController::OnWindowFrameChanged(const Rect& screen_frame) {
  if (frame_ == screen_frame) {
    return;
  }
  frame_ = screen_frame;
  if (events_ != nullptr) {
    events_->NotifyFrameChanged(view_id_, screen_frame);
  }
}

void WindowController::OnWindowControlsChanged(const std::optional<Rect>& rect) {
  if (rect == controls_) {
    return;
  }
  controls_ = rect;
  if (events_ != nullptr) {
    events_->NotifyControlsChanged(view_id_, rect);
  }
}

}  // namespace momo::embedder
