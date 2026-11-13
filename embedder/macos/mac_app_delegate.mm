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
  app->Quit();
  return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return NO;  // 最后一个窗口关闭后是否退出由 JS 决定(momo/app windowAllClosed)
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
  if (self.application != nullptr) {
    self.application->OnReopen(flag);
  }
  return NO;  // 不让 AppKit 自行还原窗口: 由 JS 处理 activate
}

- (void)applicationDidBecomeActive:(NSNotification*)notification {
  if (self.application != nullptr) {
    self.application->OnAppActiveChanged(true);
  }
}

- (void)applicationDidResignActive:(NSNotification*)notification {
  if (self.application != nullptr) {
    self.application->OnAppActiveChanged(false);
  }
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
