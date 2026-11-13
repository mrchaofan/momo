// momo_headless: 无窗口地运行 JS bundle(docs/09 §5 的 headless 测试运行器 / tools/momo-test 的引擎侧)。
//
//   momo_headless --bundle=<file.js> [--assets=<dir>] [--extensions=<dir>]
//                 [--width=800 --height=600 --dpr=1] [--timeout=30] [--screenshot=<out.png>]
//                 [--manual-frames]
// --manual-frames: window.scheduleFrame() 不触发 vsync(框架测试自行调用 onBeginFrame/onDrawFrame
// 并用假时钟驱动, 引擎再自行出帧会让时间倒退)。
//
// 引擎照常运行(四线程、定时器 vsync、软件光栅进离屏缓冲), 只是没有平台窗口。bundle 通过
// momo:services 的 momo/test 通道与运行器交互(StandardMethodCodec):
//   log(text)                         打印一行到 stdout
//   done({passed, failed})            结束;failed > 0 时进程退出码为 1
//   screenshot(path?)                 把最近一帧写成 PNG(path 缺省用 --screenshot)
// 超时(--timeout 秒)未收到 done 视为失败(退出码 2)。

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "base/event_loop.h"
#include "base/lock.h"
#include "base/logging.h"
#include "embedder/common/engine_platform.h"
#include "engine/common/standard_codec.h"
#include "engine/graphics/software_rasterizer.h"
#include "engine/shell/shell.h"
#include "engine/shell/thread_host.h"

namespace momo::embedder {
namespace {

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
    engine::graphics::PixelBuffer view = buffer_->view();
    platform::SurfaceFrame frame;
    frame.kind = platform::SurfaceKind::kSoftware;
    frame.software = {view.pixels, view.row_bytes, view.width, view.height};
    frame.size = size;
    frame.present = [this] {
      MutexLock l(mutex_);
      frames_++;
      return true;
    };
    return frame;
  }
  // 任意线程: 编码最近一帧(与 raster 写入互斥)。
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

// 永不触发的 vsync(--manual-frames)。
class ManualVsyncSource final : public platform::VsyncSource {
 public:
  void RequestFrameCallback(Callback) override {}
};

// 无窗口: 不提供原生句柄;窗口管理、剪贴板、输入法等宿主通道一律不注册(JS 调用会得到"无实现")。
class HeadlessDelegate final : public engine::Shell::Delegate {
 public:
  void* GetNativeWindowHandle() override { return nullptr; }
};

struct Options {
  std::filesystem::path bundle;
  std::filesystem::path assets;
  std::filesystem::path extensions;
  std::filesystem::path screenshot;
  double width = 800, height = 600, dpr = 1;
  double timeout_s = 30;
  bool manual_frames = false;
};

bool ParseArgs(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const size_t eq = arg.find('=');
    if (arg == "--manual-frames") {
      o->manual_frames = true;
      continue;
    }
    if (arg.rfind("--", 0) != 0 || eq == std::string_view::npos) {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return false;
    }
    const std::string key(arg.substr(2, eq - 2));
    const std::string value(arg.substr(eq + 1));
    if (key == "bundle") o->bundle = value;
    else if (key == "assets") o->assets = value;
    else if (key == "extensions") o->extensions = value;
    else if (key == "screenshot") o->screenshot = value;
    else if (key == "width") o->width = std::atof(value.c_str());
    else if (key == "height") o->height = std::atof(value.c_str());
    else if (key == "dpr") o->dpr = std::atof(value.c_str());
    else if (key == "timeout") o->timeout_s = std::atof(value.c_str());
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return false;
    }
  }
  if (o->bundle.empty()) {
    std::fprintf(stderr, "usage: momo_headless --bundle=<file.js> [--assets=dir] [--extensions=dir]"
                         " [--width=800 --height=600 --dpr=1] [--timeout=30] [--screenshot=out.png]\n");
    return false;
  }
  return true;
}

int Run(const Options& options) {
  auto loop = EventLoop::Create();
  loop->BindToCurrentThread();
  int exit_code = 2;
  {
    engine::ThreadHost threads("momo.headless");
    engine::TaskRunners runners = threads.MakeTaskRunners(loop->GetTaskRunner());
    engine::Settings settings;
    settings.bundle_path = std::filesystem::absolute(options.bundle);
    settings.assets_dir = options.assets.empty() ? settings.bundle_path.parent_path()
                                                 : std::filesystem::absolute(options.assets);
    settings.extensions_dir = options.extensions;
    settings.dev_mode = true;
    auto shell = engine::Shell::Create(runners, settings);
    if (!shell.ok()) {
      std::fprintf(stderr, "momo_headless: %s\n", std::string(shell.status().message()).c_str());
      loop->Terminate();
      loop->RunUntilIdle();
      return 2;
    }
    OffscreenSurface surface;
    HeadlessDelegate delegate;
    bool done = false;

    (*shell)->hub()->SetNativeHandler(
        "momo/test", loop->GetTaskRunner(),
        [&](engine::MessageBytes message, engine::MessageReply reply) {
          StatusOr<engine::MethodCall> call = engine::DecodeMethodCall(message);
          if (!call.ok()) {
            reply.Send(engine::EncodeErrorEnvelope("bad-call", call.status().message()));
            return;
          }
          if (call->method == "log" && call->args.Is<engine::EncodableList>()) {
            for (const auto& v : *call->args.GetIf<engine::EncodableList>()) {
              std::printf("%s\n", v.AsString().value_or("").c_str());
            }
            std::fflush(stdout);
          } else if (call->method == "log") {
            std::printf("%s\n", call->args.AsString().value_or("").c_str());
            std::fflush(stdout);
          } else if (call->method == "done") {
            const int64_t failed = call->args.Find("failed") ? call->args.Find("failed")->AsInt().value_or(1) : 1;
            const int64_t passed = call->args.Find("passed") ? call->args.Find("passed")->AsInt().value_or(0) : 0;
            std::printf("momo_headless: %lld passed, %lld failed\n", static_cast<long long>(passed),
                        static_cast<long long>(failed));
            exit_code = failed == 0 ? 0 : 1;
            done = true;
            loop->Terminate();
          } else if (call->method == "screenshot") {
            const std::filesystem::path path =
                call->args.AsString() ? std::filesystem::path(*call->args.AsString())
                                      : options.screenshot;
            StatusOr<std::vector<uint8_t>> png = surface.EncodeLastFrame();
            if (path.empty() || !png.ok()) {
              reply.Send(engine::EncodeErrorEnvelope(
                  "screenshot", path.empty() ? "no path" : png.status().message()));
              return;
            }
            std::ofstream(path, std::ios::binary)
                .write(reinterpret_cast<const char*>(png->data()),
                       static_cast<std::streamsize>(png->size()));
          } else {
            reply.Send(engine::EncodeErrorEnvelope("unknown", call->method));
            return;
          }
          reply.Send(engine::EncodeSuccessEnvelope(engine::EncodableValue()));
        });

    (*shell)->AttachView(&delegate, &surface,
                         options.manual_frames ? MakeObject<ManualVsyncSource>() : nullptr);
    (*shell)->OnMetricsChanged({{options.width * options.dpr, options.height * options.dpr}, options.dpr});
    (*shell)->OnFocusChanged(true);
    loop->GetTaskRunner()->PostDelayedTask(
        [&] {
          if (!done) {
            std::fprintf(stderr, "momo_headless: timed out after %.0fs without done()\n",
                         options.timeout_s);
            loop->Terminate();
          }
        },
        std::chrono::milliseconds(static_cast<int64_t>(options.timeout_s * 1000)));
    loop->Run();

    if (!options.screenshot.empty()) {
      if (StatusOr<std::vector<uint8_t>> png = surface.EncodeLastFrame(); png.ok()) {
        std::ofstream(options.screenshot, std::ios::binary)
            .write(reinterpret_cast<const char*>(png->data()),
                   static_cast<std::streamsize>(png->size()));
      }
    }
    (*shell)->DetachView();
    (*shell)->Shutdown();
    shell->reset();
    threads.Join();
  }
  return exit_code;
}

}  // namespace
}  // namespace momo::embedder

int main(int argc, char** argv) {
  momo::embedder::Options options;
  if (!momo::embedder::ParseArgs(argc, argv, &options)) {
    return 64;
  }
  return momo::embedder::Run(options);
}
