#include "embedder/common/window_controller.h"

#include <utility>

#include "base/logging.h"

namespace momo::embedder {

WindowController::WindowController(Client& client, engine::platform::ViewId view_id)
    : client_(client), view_id_(view_id) {}

WindowController::~WindowController() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_DCHECK(shell_ == nullptr) << "DetachEngine must be called before destruction";
  DestroyWindow();
}

Status WindowController::CreateWindow(PlatformIntegration& platform,
                                      const WindowSpec& spec) {
  MOMO_DCHECK(window_ == nullptr);
  MOMO_ASSIGN_OR_RETURN(window_, platform.CreateWindow(spec, this));
  last_metrics_ = window_->metrics();
  has_metrics_ = true;
  state_ = window_->state();
  return Status::Ok();
}

void WindowController::DestroyWindow() {
  MOMO_DCHECK(shell_ == nullptr) << "Window destroyed while the engine still renders into it";
  window_.reset();
}

PlatformWindow& WindowController::window() {
  MOMO_CHECK(window_ != nullptr);
  return *window_;
}

void WindowController::AttachEngine(engine::Shell& shell,
                                    ObjectPtr<platform::VsyncSource> vsync) {
  MOMO_CHECK(window_ != nullptr) << "AttachEngine requires a window";
  MOMO_DCHECK(shell_ == nullptr);
  shell_ = &shell;
  shell.AttachView(view_id_, this, &window_->surface(), std::move(vsync));
  if (has_metrics_) {
    shell.OnMetricsChanged(view_id_, last_metrics_);
  }
  if (focused_) {
    shell.OnFocusChanged(view_id_, true);
  }
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
}

void WindowController::OnMetricsChanged(const platform::ViewportMetrics& metrics) {
  last_metrics_ = metrics;
  has_metrics_ = true;
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
  focused_ = focused;
  if (shell_ != nullptr) {
    shell_->OnFocusChanged(view_id_, focused);
  }
  client_.OnWindowFocusChanged(view_id_, focused);
}

void WindowController::OnWindowStateChanged(const WindowStateFlags& state) {
  if (state == state_) {
    return;
  }
  state_ = state;
  UpdateRendering();
  client_.OnWindowStateChanged(view_id_, state);
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
  if (window_) {
    window_->AddPlatformView(view_id, native_view);
  }
}

void WindowController::OnPlatformViewDisposed(int64_t view_id) {
  if (window_) {
    window_->RemovePlatformView(view_id);
  }
}

void WindowController::OnPlatformViewFocusRequested(int64_t view_id) {
  if (window_) {
    window_->FocusPlatformView(view_id);
  }
}

void* WindowController::GetNativeWindowHandle() {
  return window_ ? window_->native_handle() : nullptr;
}

void WindowController::OnFirstFramePresented() {
  if (first_frame_) {
    return;
  }
  first_frame_ = true;
  UpdateRendering();
  client_.OnWindowFirstFrame(view_id_);
}

void WindowController::OnWindowFrameChanged(const Rect& screen_frame) {
  client_.OnWindowFrameChanged(view_id_, screen_frame);
}

void WindowController::OnWindowControlsChanged(const std::optional<Rect>& rect) {
  client_.OnWindowControlsChanged(view_id_, rect);
}

}  // namespace momo::embedder
