#pragma once

// MacMenus: PlatformMenus 的 AppKit 实现。仅被 embedder/macos 的 .mm 包含。
//  - 应用菜单 = NSApp.mainMenu(同一个 NSMenu 对象, 内容变化时原地更新)。没有设置(或设置为空)时是默认菜单:
//    只有应用菜单(隐藏、退出)。
//  - 上下文菜单 = -[NSMenu popUpMenuPositioningItem:atLocation:inView:], 在 run loop 的 block 里弹出(不在
//    platform EventLoop 的任务里, 见 platform_menu.h)。
//  - role = AppKit 的标准 action, target 为 nil: 沿响应链发送(quit → terminate:, copy → copy:,
//    close → performClose: …), 可用状态由 AppKit 按响应链判定(MomoContentView 在有文本输入会话时响应
//    cut/copy/paste/selectAll;原生视图如 WKWebView 自己响应)。所以 macOS 从不回调 OnMenuRoleActivated。
//  - 快捷键 = 菜单项的 key equivalent。窗口(mac_window.mm 的 MomoWindow / MomoPanel)在 performKeyEquivalent: 里
//    先交给主菜单, 所以键盘焦点在平台视图(网页)里时同样有效。
//  - 普通菜单项: tag = 菜单项 id, 点击时回调 MenuDelegate::OnMenuItemActivated。
// 线程约定: 仅 platform 线程(主线程)。

#import <Cocoa/Cocoa.h>

#include <memory>
#include <optional>
#include <string>

#include "base/macros.h"
#include "embedder/common/platform_menu.h"

@class MomoMenuTarget;

namespace momo::embedder::macos {

class MacMenus final : public PlatformMenus {
 public:
  MacMenus();  // 安装默认的应用菜单(须在 NSApplication 初始化之后)
  ~MacMenus() override;

  void SetDelegate(MenuDelegate* delegate) override;
  void SetApplicationMenu(const MenuModel* menu) override;
  void Popup(const MenuModel& menu, PlatformWindow* window, std::optional<Point> position,
             MoveOnlyFunction<void()> on_closed) override;
  void ClosePopup() override;
  StatusOr<std::unique_ptr<PlatformTray>> CreateTray(int64_t tray_id, const std::string& image_path,
                                                     TrayDelegate* delegate) override;

  // 按描述构建 NSMenu / 更新已有的 NSMenu(应用菜单、上下文菜单、托盘菜单共用)。更新时结构没变(项数、分隔线
  // 与子菜单的位置)就原地改各项的属性: 用户正打开着的菜单不受影响。
  NSMenu* BuildMenu(const MenuModel& model);
  void UpdateMenu(NSMenu* menu, const MenuModel& model);

 private:
  void ConfigureItem(NSMenuItem* ns_item, const MenuItem& item);

  MomoMenuTarget* target_;  // 普通菜单项的 target: 把点击交给 MenuDelegate
  NSMenu* popup_ = nil;     // 已请求、尚未关闭的上下文菜单

  MOMO_DISALLOW_COPY_AND_ASSIGN(MacMenus);
};

}  // namespace momo::embedder::macos
