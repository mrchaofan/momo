// Windows 入口: 只做翻译(DPI、单实例、命令行、资源目录、应用级系统事件), 启动顺序全部在 embedder/common
// (对应 macOS 的 main_mac.mm)。

#include "embedder/windows/win_util.h"  // <windows.h>(并 #undef CreateWindow)

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
  // 每个应用(打包后的 exe 名)一个实例。
  if (!delegate.AcquireSingleInstance(WinAppDelegate::ExecutablePath().stem().wstring())) {
    return 0;  // 已转发给正在运行的实例(由它重新激活)
  }

  auto config = momo::embedder::AppConfig::Parse(WinAppDelegate::CommandLineArgsUtf8());
  if (!config.ok()) {
    MOMO_LOG(ERROR) << config.status();
    return 2;
  }

  momo::embedder::Application::Options options;
  options.config = std::move(config).value();
  options.resources_dir = WinAppDelegate::ExecutableDir();

  auto app = momo::embedder::Application::Create(std::move(options),
                                                 momo::embedder::windows::CreateWinPlatformIntegration(&delegate));
  if (!app.ok()) {
    MOMO_LOG(ERROR) << app.status();
    return 1;
  }
  delegate.set_application(app->get());

  int exit_code = 1;
  if ((*app)->Launch().ok()) {
    exit_code = (*app)->Run();
  }
  delegate.set_application(nullptr);
  return exit_code;
}
