#pragma once

// 测试用 PlatformIntegration: 无任何 OS API。原生消息循环由 base 的 MockNativeLoop 模拟。

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "base/testing/mock_native_loop.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_integration.h"

namespace momo::embedder::testing {

using momo::testing::MockNativeLoop;

class MockSurface final : public platform::RenderSurface {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size) override {
    return UnimplementedError("mock");
  }
};

class MockTextInput final : public TextInputConnection {
 public:
  void Show() override { shown = true; }
  void Hide() override { shown = false; }
  void SetCaretRect(Rect) override {}
  void SetEditingState(const TextEditingState&) override {}
  bool shown = false;
};

class MockWindow final : public PlatformWindow {
 public:
  MockWindow(const WindowSpec& spec, WindowDelegate* delegate,
             std::function<void()> on_destroy)
      : spec_(spec), delegate_(delegate), on_destroy_(std::move(on_destroy)) {
    frame_.width = spec.size.width;
    frame_.height = spec.size.height;
  }
  ~MockWindow() override {
    if (on_destroy_) {
      on_destroy_();
    }
  }

  void Show() override {
    shown_ = true;
    SetState([](WindowStateFlags& s) { s.visible = true; s.minimized = false; });
  }
  Status Hide() override {
    SetState([](WindowStateFlags& s) { s.visible = false; });
    return Status::Ok();
  }
  Status Minimize() override {
    SetState([](WindowStateFlags& s) { s.minimized = true; });
    return Status::Ok();
  }
  Status Maximize() override {
    SetState([](WindowStateFlags& s) { s.maximized = true; s.minimized = false; });
    return Status::Ok();
  }
  Status Restore() override {
    SetState([](WindowStateFlags& s) {
      if (s.minimized) s.minimized = false; else s.maximized = false;
    });
    return Status::Ok();
  }
  WindowStateFlags state() const override { return state_; }
  void SetTitle(std::string_view title) override { spec_.title = std::string(title); }
  void SetMinSize(Size size) override { spec_.min_size = size; }
  Size physical_size() const override {
    return {spec_.size.width * dpr_, spec_.size.height * dpr_};
  }
  double device_pixel_ratio() const override { return dpr_; }
  platform::RenderSurface& surface() override { return surface_; }
  void SetCursor(SystemCursor cursor) override { cursor_ = cursor; }
  MockTextInput& text_input() override { return text_input_; }
  void* native_handle() const override { return nullptr; }
  void Activate() override { activations_++; }

  // 可选能力: 全部支持(记录调用)。
  uint32_t capabilities() const override {
    return kCapabilityAlwaysOnTop | kCapabilityPosition | kCapabilityResizeOptions |
           kCapabilityHiddenTitleBar | kCapabilityWindowDrag | kCapabilityMoveLoop |
           kCapabilityPointerCapture | kCapabilityVisibility | kCapabilityMinMax;
  }
  Status SetAlwaysOnTop(bool on) override {
    always_on_top_ = on;
    return Status::Ok();
  }
  StatusOr<bool> CapturePointer(int64_t sequence_id) override {
    (void)sequence_id;
    captures_++;
    return true;
  }
  void ReleasePointerCapture() override { releases_++; }
  Status SetResizeOptions(const WindowResizeOptions& options) override {
    resize_options_ = options;
    return Status::Ok();
  }
  Status SetTitleBar(const TitleBarOptions& options) override {
    title_bar_ = options;
    controls_ = options.hidden ? std::optional<Rect>(Rect{0, 0, 70, options.height > 0 ? options.height : 28})
                               : std::nullopt;
    delegate_->OnWindowControlsChanged(controls_);
    return Status::Ok();
  }
  std::optional<Rect> window_controls_rect() const override { return controls_; }
  Status StartWindowDrag(int64_t sequence_id) override {
    drags_.push_back(sequence_id);
    return Status::Ok();
  }
  Status StartMoveLoop(Point grab_offset) override {
    move_loop_ = grab_offset;
    return Status::Ok();
  }
  void EndMoveLoop() override { move_loop_.reset(); }
  Rect screen_frame() const override { return frame_; }
  Status SetPosition(Point top_left) override {
    frame_.x = top_left.x;
    frame_.y = top_left.y;
    delegate_->OnWindowFrameChanged(frame_);
    return Status::Ok();
  }

  // 模拟系统改变窗口状态(用户最小化等)。
  template <class Fn>
  void SetState(Fn change) {
    WindowStateFlags next = state_;
    change(next);
    if (next == state_) return;
    state_ = next;
    delegate_->OnWindowStateChanged(state_);
  }

  WindowDelegate* delegate() const { return delegate_; }
  const WindowSpec& spec() const { return spec_; }
  bool shown() const { return shown_; }
  void set_on_destroyed(std::function<void()> fn) { on_destroy_ = std::move(fn); }
  SystemCursor cursor() const { return cursor_; }
  int activations() const { return activations_; }
  bool always_on_top() const { return always_on_top_; }
  int captures() const { return captures_; }
  int releases() const { return releases_; }
  WindowResizeOptions resize_options() const override { return resize_options_; }
  const TitleBarOptions& title_bar() const { return title_bar_; }
  const std::vector<int64_t>& drags() const { return drags_; }
  const std::optional<Point>& move_loop() const { return move_loop_; }

 private:
  WindowSpec spec_;
  WindowDelegate* delegate_;
  std::function<void()> on_destroy_;
  MockSurface surface_;
  MockTextInput text_input_;
  double dpr_ = 2.0;
  bool shown_ = false;
  SystemCursor cursor_ = SystemCursor::kArrow;
  int activations_ = 0;
  bool always_on_top_ = false;
  int captures_ = 0;
  int releases_ = 0;
  WindowResizeOptions resize_options_;
  TitleBarOptions title_bar_;
  std::optional<Rect> controls_;
  std::vector<int64_t> drags_;
  std::optional<Point> move_loop_;
  Rect frame_{100, 100, 0, 0};
  WindowStateFlags state_{.visible = false};
};

class MockApp final : public PlatformApp {
 public:
  uint32_t capabilities() const override { return kAppCapabilityHide | kAppCapabilityActivate; }
  Status Hide() override {
    hidden = true;
    return Status::Ok();
  }
  Status Show() override {
    hidden = false;
    return Status::Ok();
  }
  Status Activate(bool steal) override {
    activations++;
    last_steal = steal;
    return Status::Ok();
  }
  bool hidden = false;
  int activations = 0;
  bool last_steal = false;
};

class MockServices final : public PlatformServices {
 public:
  StatusOr<std::string> GetClipboardText() override { return clipboard_; }
  Status SetClipboardText(std::string_view text) override {
    clipboard_ = std::string(text);
    return Status::Ok();
  }
  Status OpenUrl(std::string_view) override { return Status::Ok(); }

 private:
  std::string clipboard_;
};

// 测试持有 State 的共享引用, 在 Application 接管(并销毁)集成之后仍可检查。
struct MockPlatformState {
  MockWindow* window = nullptr;  // 主窗口(第一个创建的)
  std::vector<MockWindow*> windows;  // 全部存活的窗口(含主窗口)
  int windows_created = 0;
  int windows_destroyed = 0;
  std::function<void()> on_window_destroyed;
  bool fail_create_window = false;
};

class MockPlatformIntegration final : public PlatformIntegration {
 public:
  explicit MockPlatformIntegration(std::shared_ptr<MockPlatformState> state)
      : state_(std::move(state)) {}

  StatusOr<std::unique_ptr<PlatformWindow>> CreateWindow(
      const WindowSpec& spec, WindowDelegate* delegate) override {
    if (state_->fail_create_window) {
      return UnavailableError("mock: window creation failed");
    }
    state_->windows_created++;
    auto state = state_;
    auto window = std::make_unique<MockWindow>(spec, delegate, nullptr);
    MockWindow* raw = window.get();
    window->set_on_destroyed([state, raw] {
      state->windows_destroyed++;
      std::erase(state->windows, raw);
      if (state->window == raw) state->window = nullptr;
      if (state->on_window_destroyed) {
        state->on_window_destroyed();
      }
    });
    if (state_->window == nullptr) state_->window = raw;
    state_->windows.push_back(raw);
    return std::unique_ptr<PlatformWindow>(std::move(window));
  }

  StatusOr<ObjectPtr<platform::VsyncSource>> CreateVsyncSource(
      const PlatformWindow&) override {
    return UnimplementedError("mock");
  }
  MockServices& services() override { return services_; }
  MockApp& app() override { return app_; }
  std::vector<DisplayInfo> displays() const override { return {{}}; }
  NativeLoop& native_loop() override { return native_loop_; }

  MockNativeLoop& native() { return native_loop_; }

 private:
  std::shared_ptr<MockPlatformState> state_;
  MockNativeLoop native_loop_;
  MockServices services_;
  MockApp app_;
};

}  // namespace momo::embedder::testing
