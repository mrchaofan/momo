// momo_headless: 无窗口地运行一个应用(docs/09 §5 的 headless 测试运行器)。
//
//   momo_headless <app> [--width=800 --height=600 --dpr=1] [--timeout=30] [--screenshot=<out.png>]
//                 [--manual-frames]
// <app>: 应用路径, 目录(含 package.json)或一个 JS 文件(engine/common/app_layout.h)。
// --manual-frames: scheduleFrame() 不触发 vsync(框架测试自行调用 onBeginFrame/onDrawFrame 并用假时钟驱动,
// 引擎再自行出帧会让时间倒退)。
//
// 与 macOS / Windows 的入口一样, 它只做翻译: 把命令行变成 Application 的参数, 平台实现是 HeadlessPlatform
// (headless_platform.h: 窗口是一块离屏缓冲)。启动与关闭、窗口注册表、宿主通道(momo/app、momo/window、
// momo/textinput…)因此都是真实的那一套。另外注册运行器自己的通道 momo/test(StandardMethodCodec):
//   log(text | [text])                打印到 stdout
//   done({passed, failed})            结束;failed > 0 时进程退出码为 1
//   screenshot(path?)                 把启动窗口的最近一帧写成 PNG(path 缺省用 --screenshot)
// 超时(--timeout 秒)未收到 done 视为失败(退出码 2)。应用加载失败、process.exit(code) 按 Application 的规则退出。

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include "embedder/common/application.h"
#include "embedder/common/host_channel.h"
#include "embedder/headless/headless_platform.h"

namespace momo::embedder {
namespace {

struct Options {
  std::filesystem::path app;
  // 运行时自带的 JS 模块: 与可执行文件同目录的 builtin/(发布产物与构建目录都是这个布局)。
  std::filesystem::path builtin;
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
    if (arg.rfind("--", 0) != 0 && o->app.empty()) {
      o->app = std::string(arg);
      continue;
    }
    if (arg.rfind("--", 0) != 0 || eq == std::string_view::npos) {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return false;
    }
    const std::string key(arg.substr(2, eq - 2));
    const std::string value(arg.substr(eq + 1));
    if (key == "screenshot") o->screenshot = value;
    else if (key == "width") o->width = std::atof(value.c_str());
    else if (key == "height") o->height = std::atof(value.c_str());
    else if (key == "dpr") o->dpr = std::atof(value.c_str());
    else if (key == "timeout") o->timeout_s = std::atof(value.c_str());
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return false;
    }
  }
  if (o->app.empty()) {
    std::fprintf(stderr, "usage: momo_headless <app dir | file.js>"
                         " [--width=800 --height=600 --dpr=1] [--timeout=30] [--screenshot=out.png]\n");
    return false;
  }
  return true;
}

bool WritePng(const std::filesystem::path& path, headless::OffscreenSurface* surface) {
  if (path.empty() || surface == nullptr) {
    return false;
  }
  StatusOr<std::vector<uint8_t>> png = surface->EncodeLastFrame();
  if (!png.ok()) {
    return false;
  }
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(png->data()), static_cast<std::streamsize>(png->size()));
  return true;
}

int Run(const Options& options) {
  auto platform = std::make_unique<headless::HeadlessPlatform>(
      headless::HeadlessPlatform::Config{options.dpr, options.manual_frames});
  headless::HeadlessPlatform* host = platform.get();

  Application::Options app_options;
  app_options.config.app_path = options.app;
  app_options.config.window.size = {options.width, options.height};
  app_options.config.dev_mode = true;      // 不写 V8 code cache
  app_options.config.inspector_port = -1;  // 不开调试端口
  app_options.resources_dir = options.builtin.parent_path();  // 运行时自带的 JS 模块在它的 builtin/ 下

  Application* app = nullptr;
  std::unique_ptr<HostChannel> test_channel;
  bool done = false;
  // 结束运行: 先留下截图(窗口随后关闭), 再有序退出。
  auto finish = [&](int exit_code) {
    done = true;
    WritePng(options.screenshot, host->first_surface().get());
    app->Exit(exit_code);
  };
  // 引擎已创建、JS 尚未运行: 运行器的通道在这里就位。
  app_options.on_engine_created = [&](engine::Shell& shell) {
    using engine::EncodableValue;
    test_channel = std::make_unique<HostChannel>(shell.hub(), app->task_runner(), "momo/test");
    test_channel->Register("log", [](const EncodableValue& args) -> StatusOr<EncodableValue> {
      if (const auto* lines = args.GetIf<engine::EncodableList>()) {
        for (const EncodableValue& line : *lines) std::printf("%s\n", line.AsString().value_or("").c_str());
      } else {
        std::printf("%s\n", args.AsString().value_or("").c_str());
      }
      std::fflush(stdout);
      return EncodableValue();
    });
    test_channel->Register("done", [&](const EncodableValue& args) -> StatusOr<EncodableValue> {
      const EncodableValue* failed_value = args.Find("failed");
      const EncodableValue* passed_value = args.Find("passed");
      const int64_t failed = failed_value != nullptr ? failed_value->AsInt().value_or(1) : 1;
      const int64_t passed = passed_value != nullptr ? passed_value->AsInt().value_or(0) : 0;
      std::printf("momo_headless: %lld passed, %lld failed\n", static_cast<long long>(passed),
                  static_cast<long long>(failed));
      std::fflush(stdout);
      finish(failed == 0 ? 0 : 1);
      return EncodableValue();
    });
    test_channel->Register("screenshot", [&](const EncodableValue& args) -> StatusOr<EncodableValue> {
      const std::filesystem::path path =
          args.AsString() ? std::filesystem::path(*args.AsString()) : options.screenshot;
      if (path.empty()) return InvalidArgumentError("screenshot: no path");
      if (!WritePng(path, host->first_surface().get())) {
        return FailedPreconditionError("screenshot: no frame has been presented yet");
      }
      return EncodableValue();
    });
  };

  StatusOr<std::unique_ptr<Application>> created = Application::Create(std::move(app_options), std::move(platform));
  if (!created.ok()) {
    std::fprintf(stderr, "momo_headless: %s\n", std::string(created.status().message()).c_str());
    return 2;
  }
  app = created->get();
  if (Status status = app->Launch(); !status.ok()) {
    std::fprintf(stderr, "momo_headless: %s\n", std::string(status.message()).c_str());
    return 2;
  }
  app->task_runner()->PostDelayedTask(
      [&] {
        if (!done && app->state() != Application::State::kTerminating) {
          std::fprintf(stderr, "momo_headless: timed out after %.0fs without done()\n", options.timeout_s);
          finish(2);
        }
      },
      std::chrono::milliseconds(static_cast<int64_t>(options.timeout_s * 1000)));
  const int exit_code = app->Run();
  test_channel.reset();
  created->reset();
  return exit_code;
}

}  // namespace
}  // namespace momo::embedder

int main(int argc, char** argv) {
  momo::embedder::Options options;
  if (!momo::embedder::ParseArgs(argc, argv, &options)) {
    return 64;
  }
  std::error_code ec;
  options.builtin = std::filesystem::absolute(argv[0], ec).parent_path() / "builtin";
  return momo::embedder::Run(options);
}
