#include "embedder/common/window_controller.h"

#include <utility>

#include "base/logging.h"

namespace momo::embedder {

WindowController::WindowController(Callbacks callbacks, engine::platform::ViewId view_id)
    : callbacks_(std::move(callbacks)), view_id_(view_id) {}

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
  shell.OnWindowFrameChanged(view_id_, window_->screen_frame());
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
  if (callbacks_.on_close_requested) {
    callbacks_.on_close_requested();
    return false;
  }
  return true;
}

void WindowController::OnFocusChanged(bool focused) {
  focused_ = focused;
  if (shell_ != nullptr) {
    shell_->OnFocusChanged(view_id_, focused);
  }
}

void WindowController::OnVisibilityChanged(bool visible) {
  if (callbacks_.on_visibility_changed) {
    callbacks_.on_visibility_changed(visible);
  }
}

void WindowController::OnTextInput(const platform::TextInputEvent& event) {
  if (shell_ != nullptr) {
    shell_->OnTextInput(event);
  }
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

platform::TextInputConnection* WindowController::GetTextInput() {
  return window_ ? &window_->text_input() : nullptr;
}

void* WindowController::GetNativeWindowHandle() {
  return window_ ? window_->native_handle() : nullptr;
}

void WindowController::OnEngineSetTitle(const std::string& title) {
  if (window_) {
    window_->SetTitle(title);
  }
}

void WindowController::OnEngineSetMinSize(Size logical_size) {
  if (window_) {
    window_->SetMinSize(logical_size);
  }
}

void WindowController::OnEngineSetAlwaysOnTop(bool on) {
  if (window_) {
    window_->SetAlwaysOnTop(on);
  }
}

void WindowController::OnEngineSetResizeOptions(const platform::WindowResizeOptions& options) {
  if (window_) {
    window_->SetResizeOptions(options);
  }
}

void WindowController::OnEngineSetTitleBar(bool hidden, double traffic_center_y) {
  if (window_) {
    window_->SetTitleBar(hidden, traffic_center_y);
  }
}

void WindowController::OnEngineSetDragRegions(const std::vector<Rect>& rects) {
  if (window_) {
    window_->SetDragRegions(rects);
  }
}

void WindowController::OnWindowFrameChanged(const Rect& screen_frame) {
  if (shell_ != nullptr) {
    shell_->OnWindowFrameChanged(view_id_, screen_frame);
  }
}

void WindowController::OnEngineSetPosition(Point top_left) {
  if (window_) {
    window_->SetPosition(top_left);
  }
}

void WindowController::OnEngineActivate() {
  if (window_) {
    window_->Activate();
  }
}

void WindowController::OnEngineCapturePointer() {
  if (window_) {
    window_->CapturePointer();
  }
}

void WindowController::OnEnginePlatformViewPointer(int64_t platform_view_id,
                                                   uint64_t native_event_id,
                                                   platform::PlatformViewPointerAction action) {
  if (window_) {
    window_->DispatchPlatformViewPointer(platform_view_id, native_event_id, action);
  }
}

void WindowController::OnEngineSetCursor(platform::SystemCursor cursor) {
  if (window_) {
    window_->SetCursor(cursor);
  }
}

}  // namespace momo::embedder
