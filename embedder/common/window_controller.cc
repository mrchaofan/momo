#include "embedder/common/window_controller.h"

#include <utility>

#include "base/logging.h"

namespace momo::embedder {

StatusOr<std::unique_ptr<WindowController>> WindowController::Create(PlatformIntegration& platform,
                                                                     const WindowOptions& options,
                                                                     engine::platform::ViewId view_id,
                                                                     Bindings bindings) {
  std::unique_ptr<WindowController> controller(new WindowController(view_id, std::move(bindings)));
  MOMO_ASSIGN_OR_RETURN(controller->window_, platform.CreateWindow(options, controller.get()));
  controller->Attach();
  return controller;
}

WindowController::WindowController(engine::platform::ViewId view_id, Bindings bindings)
    : view_id_(view_id), bindings_(std::move(bindings)) {}

WindowController::~WindowController() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_DCHECK(!attached_) << "Window destroyed while the engine still renders into it";
  window_.reset();  // 销毁过程中的窗口回调仍落在完整的对象上
}

void WindowController::Attach() {
  // 以创建完成后的窗口为准(创建过程中的回调可能已经写过缓存)。
  metrics_ = window_->metrics();
  state_ = window_->state();
  frame_.reset();
  controls_ = window_->window_controls_rect();
  attached_ = true;
  engine::Shell& shell = bindings_.shell;
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
  if (attached_) {
    bindings_.shell.OnViewVisibilityChanged(view_id_, rendering);
  }
}

void WindowController::Detach() {
  if (!attached_) {
    return;
  }
  attached_ = false;
  bindings_.shell.DetachView(view_id_);
}

void WindowController::OnMetricsChanged(const platform::ViewportMetrics& metrics) {
  if (metrics == metrics_) {
    return;
  }
  metrics_ = metrics;
  if (attached_) {
    bindings_.shell.OnMetricsChanged(view_id_, metrics);
  }
}

void WindowController::OnPointerPacket(platform::PointerDataPacket packet) {
  if (attached_) {
    bindings_.shell.OnPointerPacket(view_id_, std::move(packet));
  }
}

void WindowController::OnKeyData(const platform::KeyData& data) {
  if (attached_) {
    bindings_.shell.OnKeyData(view_id_, data);
  }
}

void WindowController::OnCloseRequested() {
  // 关闭需要询问 JS(异步, ui 线程), 且销毁必须按 ARCHITECTURE.md §3.8 的顺序进行: 交给 WindowManager。
  if (attached_) {
    bindings_.close_requested();
  }
}

void WindowController::OnFocusChanged(bool focused) {
  if (focused == focused_) {
    return;
  }
  focused_ = focused;
  if (attached_) {
    bindings_.window_events.NotifyFocusChanged(view_id_, focused);
  }
}

void WindowController::OnWindowStateChanged(const WindowStateFlags& state) {
  if (state == state_) {
    return;
  }
  state_ = state;
  UpdateRendering();
  if (attached_) {
    bindings_.window_events.NotifyStateChanged(view_id_, state);
  }
}

void WindowController::OnTextInput(const TextInputEvent& event) {
  if (attached_) {
    bindings_.text_input.Send(event);
  }
}

void WindowController::OnPlatformViewFocused(int64_t view_id) {
  if (attached_) {
    bindings_.shell.OnPlatformViewFocused(view_id);
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
  if (attached_) {
    bindings_.window_events.NotifyReadyToShow(view_id_);
  }
}

void WindowController::OnWindowFrameChanged(const Rect& screen_frame) {
  if (frame_ == screen_frame) {
    return;
  }
  frame_ = screen_frame;
  if (attached_) {
    bindings_.window_events.NotifyFrameChanged(view_id_, screen_frame);
  }
}

void WindowController::OnWindowControlsChanged(const std::optional<Rect>& rect) {
  if (rect == controls_) {
    return;
  }
  controls_ = rect;
  if (attached_) {
    bindings_.window_events.NotifyControlsChanged(view_id_, rect);
  }
}

}  // namespace momo::embedder
