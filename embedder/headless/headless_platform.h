#pragma once

// 无窗口的平台实现(momo_headless 用): 和 macOS / Windows 一样是一个 PlatformIntegration, 跑在同一个
// Application 上——启动与关闭顺序、窗口注册表、全部宿主通道都是真实的那一套, 只有"窗口"是内存里的
// 一块离屏缓冲。原生消息循环是 QueueNativeLoop(本线程上的任务队列)。
// 线程约定: 与其他平台实现相同(platform 线程;surface 由 raster 线程使用)。

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/lock.h"
#include "base/queue_native_loop.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_integration.h"
#include "engine/graphics/software_rasterizer.h"

namespace momo::embedder::headless {

// 离屏软件 surface: raster 线程画进自有缓冲, 保留最近一帧供截图。
class OffscreenSurface final : public platform::RenderSurface {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size size) override {
    const int w = static_cast<int>(size.width);
    const int h = static_cast<int>(size.height);
    if (w <= 0 || h <= 0) {
      return InvalidArgumentError("empty frame");
    }
    MutexLock lock(mutex_);
    if (!buffer_ || buffer_->width() != w || buffer_->height() != h) {
      buffer_ = std::make_unique<engine::graphics::OwnedPixelBuffer>(w, h);
    }
    platform::SurfaceFrame frame;
    frame.kind = platform::SurfaceKind::kSoftware;
    frame.software = buffer_->view();
    frame.size = size;
    frame.present = [this] {
      MutexLock l(mutex_);
      frames_++;
      return true;
    };
    return frame;
  }
  // 任意线程: 把最近一帧编码为 PNG(与 raster 写入互斥)。
  StatusOr<std::vector<uint8_t>> EncodeLastFrame() {
    MutexLock lock(mutex_);
    if (!buffer_ || frames_ == 0) {
      return FailedPreconditionError("no frame has been presented yet");
    }
    return engine::graphics::EncodePng(buffer_->view());
  }

 private:
  Mutex mutex_;
  std::unique_ptr<engine::graphics::OwnedPixelBuffer> buffer_;
  uint64_t frames_ = 0;
};

// 永不触发的 vsync(--manual-frames: 帧由测试自己驱动)。
class ManualVsyncSource final : public platform::VsyncSource {
 public:
  void RequestFrameCallback(Callback) override {}
};

class HeadlessPlatform;

// 一个"窗口": 尺寸、状态、屏幕位置与一块离屏缓冲。没有用户, 所以状态只随宿主通道的调用变化。
class HeadlessWindow final : public PlatformWindow {
 public:
  HeadlessWindow(HeadlessPlatform& platform, const WindowOptions& options, double dpr,
                 WindowDelegate* delegate, std::shared_ptr<OffscreenSurface> surface)
      : platform_(platform),
        delegate_(delegate),
        surface_(std::move(surface)),
        dpr_(dpr),
        frame_{options.position ? options.position->x : 0, options.position ? options.position->y : 0,
               options.size.width, options.size.height} {}
  ~HeadlessWindow() override;

  void Show() override {
    (void)SetState([](WindowStateFlags& s) {
      s.visible = true;
      s.minimized = false;
    });
    Activate();
  }
  void Activate() override;
  Status Hide() override { return SetState([](WindowStateFlags& s) { s.visible = false; }); }
  Status Minimize() override { return SetState([](WindowStateFlags& s) { s.minimized = true; }); }
  Status Maximize() override {
    return SetState([](WindowStateFlags& s) {
      s.maximized = true;
      s.minimized = false;
    });
  }
  Status Restore() override {
    return SetState([](WindowStateFlags& s) { (s.minimized ? s.minimized : s.maximized) = false; });
  }
  WindowStateFlags state() const override { return state_; }
  void SetTitle(std::string_view) override {}
  void SetMinSize(Size) override {}
  Size physical_size() const override { return {frame_.width * dpr_, frame_.height * dpr_}; }
  double device_pixel_ratio() const override { return dpr_; }
  platform::RenderSurface& surface() override { return *surface_; }
  void SetCursor(SystemCursor) override {}
  TextInputConnection& text_input() override { return text_input_; }
  void* native_handle() const override { return nullptr; }
  Rect screen_frame() const override { return frame_; }
  Status SetPosition(Point top_left) override {
    frame_.x = top_left.x;
    frame_.y = top_left.y;
    delegate_->OnWindowFrameChanged(frame_);
    return Status::Ok();
  }

  // 系统焦点的得失(由 HeadlessPlatform 决定: 最近一次 Show / Activate 的窗口持有焦点)。
  void SetFocused(bool focused) { delegate_->OnFocusChanged(focused); }

 private:
  class NoTextInput final : public TextInputConnection {
   public:
    void Show() override {}
    void Hide() override {}
    void SetCaretRect(Rect) override {}
    void SetEditingState(const TextEditingState&) override {}
  };

  template <typename Fn>
  Status SetState(Fn change) {
    change(state_);
    delegate_->OnWindowStateChanged(state_);
    return Status::Ok();
  }

  HeadlessPlatform& platform_;
  WindowDelegate* const delegate_;
  const std::shared_ptr<OffscreenSurface> surface_;
  NoTextInput text_input_;
  const double dpr_;
  Rect frame_;
  WindowStateFlags state_{.visible = false};
};

class HeadlessPlatform final : public PlatformIntegration {
 public:
  struct Config {
    double device_pixel_ratio = 1;
    bool manual_frames = false;  // 不提供帧时钟: scheduleFrame 之后不会自己出帧
  };
  explicit HeadlessPlatform(Config config) : config_(config) {}

  StatusOr<std::unique_ptr<PlatformWindow>> CreateWindow(const WindowOptions& options,
                                                         WindowDelegate* delegate) override {
    auto surface = std::make_shared<OffscreenSurface>();
    if (first_surface_ == nullptr) {
      first_surface_ = surface;
    }
    return std::unique_ptr<PlatformWindow>(std::make_unique<HeadlessWindow>(
        *this, options, config_.device_pixel_ratio, delegate, std::move(surface)));
  }
  StatusOr<ObjectPtr<platform::VsyncSource>> CreateVsyncSource() override {
    if (config_.manual_frames) {
      return ObjectPtr<platform::VsyncSource>(MakeObject<ManualVsyncSource>());
    }
    return UnimplementedError("headless: no display; the engine uses its timer");
  }
  PlatformServices& services() override { return services_; }
  PlatformApp& app() override { return app_; }
  std::vector<DisplayInfo> displays() const override { return {}; }
  NativeLoop& native_loop() override { return native_loop_; }

  // 第一个窗口(启动窗口)的画面: 截图用。窗口关闭后仍可读取它的最后一帧。
  const std::shared_ptr<OffscreenSurface>& first_surface() const { return first_surface_; }

  // 窗口的系统焦点: 同一时刻至多一个窗口持有。
  void Focus(HeadlessWindow* window) {
    if (focused_ == window) return;
    if (focused_ != nullptr) focused_->SetFocused(false);
    focused_ = window;
    window->SetFocused(true);
  }
  void Forget(HeadlessWindow* window) {
    if (focused_ == window) focused_ = nullptr;
  }

 private:
  // 剪贴板只在进程内;没有浏览器可打开。
  class Services final : public PlatformServices {
   public:
    StatusOr<std::string> GetClipboardText() override { return clipboard_; }
    Status SetClipboardText(std::string_view utf8) override {
      clipboard_ = std::string(utf8);
      return Status::Ok();
    }
    Status OpenUrl(std::string_view) override { return UnimplementedError("headless: cannot open a URL"); }

   private:
    std::string clipboard_;
  };

  const Config config_;
  QueueNativeLoop native_loop_;
  Services services_;
  PlatformApp app_;  // 隐藏、显示、激活: 都不支持(默认实现)
  std::shared_ptr<OffscreenSurface> first_surface_;
  HeadlessWindow* focused_ = nullptr;
};

inline HeadlessWindow::~HeadlessWindow() { platform_.Forget(this); }
inline void HeadlessWindow::Activate() { platform_.Focus(this); }

}  // namespace momo::embedder::headless
