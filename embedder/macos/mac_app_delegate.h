#pragma once

// MacAppDelegate: NSApplicationDelegate, 把应用级生命周期事件翻译给 embedder::Application。
//  - Cmd+Q / Dock 退出 → applicationShouldTerminate → Application::Quit(JS 可取消)
//  - 隐藏/显示 → Application::OnAppVisibilityChanged(Suspended ⇄ Running)
//  - 成为/不再是前台应用 → Application::OnAppActiveChanged
//  - 点击 Dock 图标重新激活 → Application::OnReopen(JS 的 app 'activate' 事件)
// 所有窗口关闭后应用不退出(applicationShouldTerminateAfterLastWindowClosed = NO), 由 JS 决定。
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
