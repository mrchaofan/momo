#pragma once

// 测试用 PlatformIntegration: 无任何 OS API, 记录调用供单测检查。原生消息循环是 base 的 MockNativeLoop
// (QueueNativeLoop 加调用计数)。真正运行应用的无窗口平台是 embedder/headless/headless_platform.h。

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
  MockWindow(const WindowOptions& spec, WindowDelegate* delegate,
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

  // 可选操作: 全部支持(记录调用)。
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
  const WindowOptions& spec() const { return spec_; }
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
  WindowOptions spec_;
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

// 菜单与托盘的记录: 测试经 MockPlatformState::menus 检查, 并以 delegate 模拟用户点击。
struct MockTrayState {
  std::string image;
  std::string tooltip;
  std::optional<MenuModel> menu;
  TrayDelegate* delegate = nullptr;
};

struct MockMenusState {
  MenuDelegate* delegate = nullptr;
  std::optional<MenuModel> application_menu;
  int application_menu_sets = 0;
  std::vector<MenuModel> popups;
  std::optional<Point> popup_position;
  PlatformWindow* popup_window = nullptr;
  MoveOnlyFunction<void()> popup_closed;  // 非空 = 有打开着的菜单
  std::map<int64_t, MockTrayState> trays;  // 存活的图标
};

class MockTray final : public PlatformTray {
 public:
  MockTray(std::shared_ptr<MockMenusState> state, int64_t id) : state_(std::move(state)), id_(id) {}
  ~MockTray() override { state_->trays.erase(id_); }
  Status SetImage(const std::string& path) override {
    if (path.empty()) return InvalidArgumentError("mock: no image");
    state_->trays[id_].image = path;
    return Status::Ok();
  }
  void SetToolTip(std::string_view text) override { state_->trays[id_].tooltip = std::string(text); }
  void SetContextMenu(const MenuModel* menu) override {
    state_->trays[id_].menu = menu != nullptr ? std::optional<MenuModel>(*menu) : std::nullopt;
  }
  std::optional<Rect> bounds() const override { return Rect{1, 2, 3, 4}; }

 private:
  std::shared_ptr<MockMenusState> state_;
  const int64_t id_;
};

class MockMenus final : public PlatformMenus {
 public:
  explicit MockMenus(std::shared_ptr<MockMenusState> state) : state_(std::move(state)) {}
  void SetDelegate(MenuDelegate* delegate) override { state_->delegate = delegate; }
  void SetApplicationMenu(const MenuModel* menu) override {
    state_->application_menu = menu != nullptr ? std::optional<MenuModel>(*menu) : std::nullopt;
    state_->application_menu_sets++;
  }
  void Popup(const MenuModel& menu, PlatformWindow* window, std::optional<Point> position,
             MoveOnlyFunction<void()> on_closed) override {
    ClosePopup();
    state_->popups.push_back(menu);
    state_->popup_window = window;
    state_->popup_position = position;
    state_->popup_closed = std::move(on_closed);
  }
  // 打开着的菜单关闭(测试也用它模拟用户关闭菜单)。
  void ClosePopup() override {
    if (auto closed = std::move(state_->popup_closed)) {
      state_->popup_closed = nullptr;
      closed();
    }
  }
  StatusOr<std::unique_ptr<PlatformTray>> CreateTray(int64_t tray_id, const std::string& image_path,
                                                     TrayDelegate* delegate) override {
    auto tray = std::make_unique<MockTray>(state_, tray_id);
    MOMO_RETURN_IF_ERROR(tray->SetImage(image_path));
    state_->trays[tray_id].delegate = delegate;
    return std::unique_ptr<PlatformTray>(std::move(tray));
  }

 private:
  std::shared_ptr<MockMenusState> state_;
};

// 测试持有 State 的共享引用, 在 Application 接管(并销毁)集成之后仍可检查。
struct MockPlatformState {
  std::shared_ptr<MockMenusState> menus = std::make_shared<MockMenusState>();
  MockWindow* window = nullptr;  // 主窗口(第一个创建的)
  std::vector<MockWindow*> windows;  // 全部存活的窗口(含主窗口)
  int windows_created = 0;
  int windows_destroyed = 0;
  int vsync_sources_created = 0;
  const MockWindow* vsync_window = nullptr;  // 帧时钟跟随的窗口(SetVsyncWindow 的最近一次取值)
  std::function<void()> on_window_destroyed;
  bool fail_create_window = false;
};

class MockPlatformIntegration final : public PlatformIntegration {
 public:
  explicit MockPlatformIntegration(std::shared_ptr<MockPlatformState> state)
      : state_(std::move(state)) {}

  StatusOr<std::unique_ptr<PlatformWindow>> CreateWindow(
      const WindowOptions& spec, WindowDelegate* delegate) override {
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

  StatusOr<ObjectPtr<platform::VsyncSource>> CreateVsyncSource() override {
    state_->vsync_sources_created++;
    return UnimplementedError("mock");
  }
  void SetVsyncWindow(const PlatformWindow* window) override {
    state_->vsync_window = static_cast<const MockWindow*>(window);
  }
  MockServices& services() override { return services_; }
  MockApp& app() override { return app_; }
  PlatformMenus* menus() override { return &menus_; }
  std::vector<DisplayInfo> displays() const override { return {{}}; }
  NativeLoop& native_loop() override { return native_loop_; }

  MockNativeLoop& native() { return native_loop_; }

 private:
  std::shared_ptr<MockPlatformState> state_;
  MockNativeLoop native_loop_;
  MockServices services_;
  MockApp app_;
  MockMenus menus_{state_->menus};
};

}  // namespace momo::embedder::testing
