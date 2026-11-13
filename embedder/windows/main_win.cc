// Windows 入口: 只做翻译(DPI、单实例、命令行、资源目录), 启动顺序全部在 embedder/common。
// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。

#include <windows.h>
// <windows.h> 的 CreateWindow 宏会改写 Application/WindowController/PlatformIntegration 中同名成员。
#ifdef CreateWindow
#undef CreateWindow
#endif

#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "embedder/common/app_config.h"
#include "embedder/common/application.h"
#include "embedder/windows/win_app_delegate.h"
#include "embedder/windows/win_platform_integration.h"

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  using momo::embedder::WinAppDelegate;
  // 必须先于任何窗口创建。
  WinAppDelegate::EnablePerMonitorDpiAwareness();

  WinAppDelegate delegate;
  if (!delegate.AcquireSingleInstance(L"momo_app")) {
    return 0;  // 命令行已转发给正在运行的实例
  }

  auto config = momo::embedder::AppConfig::Parse(WinAppDelegate::CommandLineArgsUtf8());
  if (!config.ok()) {
    MOMO_LOG(ERROR) << config.status();
    return 2;
  }

  momo::embedder::Application::Options options;
  options.config = std::move(config).value();
  options.resources_dir = WinAppDelegate::ExecutableDir();

  auto app = momo::embedder::Application::Create(
      std::move(options), momo::embedder::windows::CreateWinPlatformIntegration());
  if (!app.ok()) {
    MOMO_LOG(ERROR) << app.status();
    return 1;
  }
  momo::embedder::Application* application = app->get();
  // 再次启动应用: 激活已有窗口(参数目前只记录日志;打开文件等语义待定义)。
  delegate.SetSecondInstanceHandler([application](std::vector<std::string>) {
    momo::embedder::WindowController* controller = application->windows().front_window();
    if (controller != nullptr && controller->has_window()) {
      WinAppDelegate::ActivateWindow(controller->window().native_handle());
    }
  });

  int exit_code = 1;
  if ((*app)->Launch().ok()) {
    exit_code = (*app)->Run();
  }
  delegate.SetSecondInstanceHandler(nullptr);
  return exit_code;
}
