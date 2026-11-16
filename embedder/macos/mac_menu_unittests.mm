// MacMenus / MacTray: MenuModel → NSMenu(标题、快捷键、role 的标准 action、点击回调)、应用菜单、上下文菜单的
// 关闭回调、状态项。不弹出真实的菜单(跟踪循环需要用户操作)。

#import <Cocoa/Cocoa.h>

#include <string>
#include <vector>

#import "embedder/macos/mac_menu.h"
#include "engine/platform/input_types.h"
#include "gtest/gtest.h"

// NSString 按内容比较。
#define EXPECT_NSEQ(actual, expected) EXPECT_TRUE([(actual) isEqualToString:(expected)]) << [(actual) UTF8String]

namespace momo::embedder::macos {
namespace {

class RecordingDelegate final : public MenuDelegate, public TrayDelegate {
 public:
  void OnMenuItemActivated(int64_t id) override { items.push_back(id); }
  void OnMenuRoleActivated(MenuRole) override { roles++; }
  void OnTrayClicked(int64_t, TrayClick, const std::optional<Rect>&) override {}
  std::vector<int64_t> items;
  int roles = 0;
};

class MacMenusTest : public ::testing::Test {
 protected:
  MacMenusTest() { [NSApplication sharedApplication]; }
  static void Click(NSMenuItem* item) { [NSApp sendAction:item.action to:item.target from:item]; }
};

TEST_F(MacMenusTest, DefaultApplicationMenuHasHideAndQuit) {
  MacMenus menus;
  NSMenu* app = NSApp.mainMenu.itemArray.firstObject.submenu;
  ASSERT_EQ(app.numberOfItems, 3);
  EXPECT_EQ([app itemAtIndex:0].action, @selector(hide:));
  EXPECT_TRUE([[app itemAtIndex:0].title hasPrefix:@"Hide "]);
  EXPECT_TRUE([app itemAtIndex:1].separatorItem);
  EXPECT_EQ([app itemAtIndex:2].action, @selector(terminate:));
  EXPECT_NSEQ([app itemAtIndex:2].keyEquivalent, @"q");
}

TEST_F(MacMenusTest, BuildsItemsRolesAndAccelerators) {
  MacMenus menus;
  RecordingDelegate delegate;
  menus.SetDelegate(&delegate);

  MenuItem file{.type = MenuItemType::kSubmenu, .label = "File"};
  file.submenu.push_back({.id = 11, .label = "New Tab",
                          .accelerator = MenuAccelerator{"t", platform::kModifierMeta | platform::kModifierShift}});
  file.submenu.push_back({.id = 12, .type = MenuItemType::kCheckbox, .label = "Pinned", .checked = true});
  file.submenu.push_back({.id = 13, .label = "Disabled", .enabled = false});
  file.submenu.push_back({.id = 14, .label = "Reload", .accelerator = MenuAccelerator{"F5", 0}});
  file.submenu.push_back({.type = MenuItemType::kSeparator});
  file.submenu.push_back({.role = MenuRole::kClose});
  file.submenu.push_back({.role = MenuRole::kCopy, .label = "Kopieren", .accelerator = MenuAccelerator{"k", platform::kModifierControl}});
  file.submenu.push_back({.role = MenuRole::kQuit, .enabled = false});
  MenuItem window{.type = MenuItemType::kSubmenu, .role = MenuRole::kWindowMenu};
  window.submenu.push_back({.role = MenuRole::kMinimize});
  MenuItem app{.type = MenuItemType::kSubmenu, .role = MenuRole::kAppMenu};
  app.submenu.push_back({.type = MenuItemType::kSubmenu, .role = MenuRole::kServices});
  const MenuModel model{app, file, window};
  menus.SetApplicationMenu(&model);

  NSMenu* main = NSApp.mainMenu;
  ASSERT_EQ(main.numberOfItems, 3);
  NSMenu* ns_file = [main itemAtIndex:1].submenu;
  EXPECT_NSEQ(ns_file.title, @"File");
  ASSERT_EQ(ns_file.numberOfItems, 8);

  NSMenuItem* new_tab = [ns_file itemAtIndex:0];
  EXPECT_NSEQ(new_tab.title, @"New Tab");
  EXPECT_NSEQ(new_tab.keyEquivalent, @"t");
  EXPECT_EQ(static_cast<NSUInteger>(new_tab.keyEquivalentModifierMask),
            static_cast<NSUInteger>(NSEventModifierFlagCommand | NSEventModifierFlagShift));
  Click(new_tab);
  EXPECT_EQ(delegate.items, (std::vector<int64_t>{11}));

  EXPECT_EQ([ns_file itemAtIndex:1].state, NSControlStateValueOn);
  EXPECT_EQ([ns_file itemAtIndex:2].action, nullptr) << "a disabled item has no action: AppKit greys it out";
  const unichar f5 = NSF5FunctionKey;
  EXPECT_NSEQ([ns_file itemAtIndex:3].keyEquivalent, [NSString stringWithCharacters:&f5 length:1]);
  EXPECT_TRUE([ns_file itemAtIndex:4].separatorItem);

  // role: 标准 action 沿响应链发送(target 为空), 标题与快捷键缺省取系统的。
  NSMenuItem* close = [ns_file itemAtIndex:5];
  EXPECT_EQ(close.action, @selector(performClose:));
  EXPECT_EQ(close.target, nil);
  EXPECT_NSEQ(close.title, @"Close Window");
  EXPECT_NSEQ(close.keyEquivalent, @"w");
  NSMenuItem* copy = [ns_file itemAtIndex:6];
  EXPECT_EQ(copy.action, @selector(copy:));
  EXPECT_NSEQ(copy.title, @"Kopieren");
  EXPECT_NSEQ(copy.keyEquivalent, @"k");
  EXPECT_EQ(static_cast<NSUInteger>(copy.keyEquivalentModifierMask), static_cast<NSUInteger>(NSEventModifierFlagControl));
  EXPECT_EQ([ns_file itemAtIndex:7].action, nullptr);

  // 服务菜单与窗口菜单交给 AppKit 维护。
  EXPECT_EQ(NSApp.windowsMenu, [main itemAtIndex:2].submenu);
  EXPECT_NSEQ(NSApp.windowsMenu.title, @"Window");
  EXPECT_EQ(NSApp.servicesMenu, [[main itemAtIndex:0].submenu itemAtIndex:0].submenu);

  // 结构不变的更新是原地的: NSMenu 与 NSMenuItem 对象不变, 只改属性。
  MenuModel renamed = model;
  renamed[1].submenu[0].label = "New Window";
  renamed[1].submenu[1].checked = false;
  renamed[1].submenu[2].enabled = true;
  menus.SetApplicationMenu(&renamed);
  EXPECT_EQ(NSApp.mainMenu, main);
  EXPECT_EQ([ns_file itemAtIndex:0], new_tab);
  EXPECT_NSEQ(new_tab.title, @"New Window");
  EXPECT_EQ([ns_file itemAtIndex:1].state, NSControlStateValueOff);
  EXPECT_EQ([ns_file itemAtIndex:2].action, @selector(itemClicked:));
  // 结构变了(少了一项): 这个菜单的各项重建。
  renamed[1].submenu.erase(renamed[1].submenu.begin());
  menus.SetApplicationMenu(&renamed);
  EXPECT_EQ(ns_file.numberOfItems, 7);
  EXPECT_NSEQ([ns_file itemAtIndex:0].title, @"Pinned");

  // 恢复默认菜单;解除 delegate 之后点击不再送达。
  menus.SetApplicationMenu(nullptr);
  EXPECT_EQ(NSApp.mainMenu.numberOfItems, 1);
  menus.SetDelegate(nullptr);
  Click(new_tab);
  EXPECT_EQ(delegate.items.size(), 1u);
  EXPECT_EQ(delegate.roles, 0);
}

TEST_F(MacMenusTest, AClosedPopupReportsClosedWithoutOpening) {
  MacMenus menus;
  const MenuModel model{{.id = 1, .label = "Copy"}};
  int first_closed = 0, second_closed = 0;
  menus.Popup(model, nullptr, std::nullopt, [&] { first_closed++; });
  menus.Popup(model, nullptr, std::nullopt, [&] { second_closed++; });  // 取代前一个
  menus.ClosePopup();
  EXPECT_EQ(first_closed + second_closed, 0) << "never reported from inside the call";
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
  EXPECT_EQ(first_closed, 1);
  EXPECT_EQ(second_closed, 1);
}

TEST_F(MacMenusTest, TrayLoadsImagesAndTakesAMenu) {
  // 16x16 的 PNG;文件名以 Template 结尾 → 模板图像。
  NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                    pixelsWide:16
                                                                    pixelsHigh:16
                                                                 bitsPerSample:8
                                                               samplesPerPixel:4
                                                                      hasAlpha:YES
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:0
                                                                  bitsPerPixel:0];
  NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  NSString* dir = NSTemporaryDirectory();
  NSString* plain = [dir stringByAppendingPathComponent:@"momo_tray_test.png"];
  NSString* templ = [dir stringByAppendingPathComponent:@"momo_tray_testTemplate.png"];
  ASSERT_TRUE([png writeToFile:plain atomically:YES]);
  ASSERT_TRUE([png writeToFile:templ atomically:YES]);

  MacMenus menus;
  RecordingDelegate delegate;
  EXPECT_FALSE(menus.CreateTray(1, "/nonexistent/tray.png", &delegate).ok());
  auto tray = menus.CreateTray(1, plain.UTF8String, &delegate);
  ASSERT_TRUE(tray.ok()) << tray.status();
  EXPECT_TRUE((*tray)->SetImage(templ.UTF8String).ok());
  EXPECT_FALSE((*tray)->SetImage("/nonexistent/other.png").ok());
  (*tray)->SetToolTip("Momo");
  EXPECT_TRUE((*tray)->SetTitle("3").ok());
  const MenuModel model{{.id = 5, .label = "Show"}, {.role = MenuRole::kQuit}};
  (*tray)->SetContextMenu(&model);
  (*tray)->SetContextMenu(nullptr);
  tray->reset();  // 移除状态项
  [NSFileManager.defaultManager removeItemAtPath:plain error:nil];
  [NSFileManager.defaultManager removeItemAtPath:templ error:nil];
}

}  // namespace
}  // namespace momo::embedder::macos
