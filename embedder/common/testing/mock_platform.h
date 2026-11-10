#pragma once

// 测试用 PlatformIntegration: 无任何 OS API。原生消息循环由 base 的 MockNativeLoop 模拟。

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

class MockTextInput final : public platform::TextInputConnection {
 public:
  void Show() override {}
  void Hide() override {}
  void SetCaretRect(Rect) override {}
  void SetEditingState(const platform::TextEditingState&) override {}
};

class MockWindow final : public PlatformWindow {
 public:
  MockWindow(const WindowSpec& spec, WindowDelegate* delegate,
             std::function<void()> on_destroy)
      : spec_(spec), delegate_(delegate), on_destroy_(std::move(on_destroy)) {}
  ~MockWindow() override {
    if (on_destroy_) {
      on_destroy_();
    }
  }

  void Show() override { shown_ = true; }
  void SetTitle(std::string_view title) override { spec_.title = std::string(title); }
  void SetMinSize(Size size) override { spec_.min_size = size; }
  Size physical_size() const override {
    return {spec_.size.width * dpr_, spec_.size.height * dpr_};
  }
  double device_pixel_ratio() const override { return dpr_; }
  platform::RenderSurface& surface() override { return surface_; }
  void SetCursor(platform::SystemCursor) override {}
  platform::TextInputConnection& text_input() override { return text_input_; }
  void* native_handle() const override { return nullptr; }

  WindowDelegate* delegate() const { return delegate_; }
  const WindowSpec& spec() const { return spec_; }
  bool shown() const { return shown_; }
  void set_on_destroyed(std::function<void()> fn) { on_destroy_ = std::move(fn); }

 private:
  WindowSpec spec_;
  WindowDelegate* delegate_;
  std::function<void()> on_destroy_;
  MockSurface surface_;
  MockTextInput text_input_;
  double dpr_ = 2.0;
  bool shown_ = false;
};

class MockServices final : public platform::PlatformServices {
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
  platform::PlatformServices& services() override { return services_; }
  std::vector<DisplayInfo> displays() const override { return {{}}; }
  NativeLoop& native_loop() override { return native_loop_; }

  MockNativeLoop& native() { return native_loop_; }

 private:
  std::shared_ptr<MockPlatformState> state_;
  MockNativeLoop native_loop_;
  MockServices services_;
};

}  // namespace momo::embedder::testing
