// macOS 入口: 只做翻译(参数、资源目录、NSApplication 委托), 启动顺序全部在 embedder/common。

#import <Cocoa/Cocoa.h>

#include <string>
#include <vector>

#include "embedder/common/app_config.h"
#include "embedder/common/application.h"
#import "embedder/macos/mac_app_delegate.h"
#include "base/logging.h"
#include "embedder/macos/mac_platform_integration.h"

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);
    auto config = momo::embedder::AppConfig::Parse(args);
    if (!config.ok()) {
      MOMO_LOG(ERROR) << config.status();
      return 2;
    }

    auto platform = momo::embedder::macos::CreateMacPlatformIntegration();  // 初始化 NSApp
    MacAppDelegate* delegate = [[MacAppDelegate alloc] init];
    NSApp.delegate = delegate;
    [MacAppDelegate installMainMenuWithAppName:@(config->window.title.c_str())];

    momo::embedder::Application::Options options;
    options.config = std::move(config).value();
    options.resources_dir = NSBundle.mainBundle.resourcePath.fileSystemRepresentation;

    auto app = momo::embedder::Application::Create(std::move(options), std::move(platform));
    if (!app.ok()) {
      MOMO_LOG(ERROR) << app.status();
      return 1;
    }
    delegate.application = app->get();

    int exit_code = 1;
    if ((*app)->Launch().ok()) {
      exit_code = (*app)->Run();
    }
    delegate.application = nullptr;
    NSApp.delegate = nil;
    return exit_code;
  }
}
