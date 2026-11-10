#pragma once

// MacAppDelegate: NSApplicationDelegate, 把应用级生命周期事件翻译给 embedder::Application。
//  - Cmd+Q / Dock 退出 → applicationShouldTerminate → Application::RequestTerminate(JS 可拦截)
//  - 隐藏/显示 → Application::OnAppVisibilityChanged(Suspended ⇄ Running)
// 同时安装最小主菜单(应用菜单 + 退出)。完整菜单属于 platform/menu(后续里程碑)。

#import <Cocoa/Cocoa.h>

namespace momo::embedder {
class Application;
}

@interface MacAppDelegate : NSObject <NSApplicationDelegate>
// 非拥有;Application 由 main 持有, 在 delegate 解绑后才销毁。
@property(nonatomic, assign) momo::embedder::Application* application;
+ (void)installMainMenuWithAppName:(NSString*)appName;
@end
