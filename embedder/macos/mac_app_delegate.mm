#import "embedder/macos/mac_app_delegate.h"

#include "embedder/common/application.h"

using momo::embedder::Application;

@implementation MacAppDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  Application* app = self.application;
  if (app == nullptr || app->state() == Application::State::kTerminating) {
    return NSTerminateNow;
  }
  // 由我们自己按 docs/01 §4 有序关闭并 [NSApp stop:], 不让 AppKit 直接 exit()。
  app->RequestTerminate();
  return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return NO;  // 关窗由 WindowController → Application 处理
}

- (void)applicationDidHide:(NSNotification*)notification {
  if (self.application != nullptr) {
    self.application->OnAppVisibilityChanged(false);
  }
}

- (void)applicationDidUnhide:(NSNotification*)notification {
  if (self.application != nullptr) {
    self.application->OnAppVisibilityChanged(true);
  }
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
  return YES;
}

+ (void)installMainMenuWithAppName:(NSString*)appName {
  NSMenu* mainMenu = [[NSMenu alloc] init];
  NSMenuItem* appItem = [[NSMenuItem alloc] init];
  [mainMenu addItem:appItem];

  NSMenu* appMenu = [[NSMenu alloc] init];
  [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                     action:@selector(hide:)
              keyEquivalent:@"h"];
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                     action:@selector(terminate:)
              keyEquivalent:@"q"];
  appItem.submenu = appMenu;
  NSApp.mainMenu = mainMenu;
}

@end
