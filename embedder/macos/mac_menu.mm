#import "embedder/macos/mac_menu.h"

#include <string_view>
#include <utility>

#include "embedder/common/engine_platform.h"
#include "embedder/macos/mac_tray.h"
#include "engine/platform/input_types.h"

using momo::embedder::MenuDelegate;

@interface MomoMenuTarget : NSObject
// 非拥有;由 MenuChannel 设置与解除。
@property(nonatomic, assign) MenuDelegate* delegate;
- (void)itemClicked:(NSMenuItem*)sender;
@end

@implementation MomoMenuTarget
- (void)itemClicked:(NSMenuItem*)sender {
  if (self.delegate != nullptr) {
    self.delegate->OnMenuItemActivated(sender.tag);
  }
}
@end

namespace momo::embedder::macos {

namespace {

NSString* ToNSString(std::string_view utf8) {
  NSString* s = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
  return s ?: @"";
}

NSString* AppName() {
  return NSRunningApplication.currentApplication.localizedName ?: NSProcessInfo.processInfo.processName;
}

// role → AppKit 的标准 action、标准标题与快捷键。action 为空的是整个子菜单。
struct RoleInfo {
  MenuRole role;
  SEL action;
  NSString* title;  // "%@" 处填应用名
  NSString* key;
  NSEventModifierFlags modifiers;
};

const RoleInfo* FindRole(MenuRole role) {
  static constexpr NSEventModifierFlags kCmd = NSEventModifierFlagCommand;
  static const RoleInfo kRoles[] = {
      {MenuRole::kAbout, @selector(orderFrontStandardAboutPanel:), @"About %@", @"", 0},
      {MenuRole::kServices, nullptr, @"Services", @"", 0},
      {MenuRole::kHide, @selector(hide:), @"Hide %@", @"h", kCmd},
      {MenuRole::kHideOthers, @selector(hideOtherApplications:), @"Hide Others", @"h",
       kCmd | NSEventModifierFlagOption},
      {MenuRole::kUnhide, @selector(unhideAllApplications:), @"Show All", @"", 0},
      {MenuRole::kQuit, @selector(terminate:), @"Quit %@", @"q", kCmd},
      {MenuRole::kUndo, @selector(undo:), @"Undo", @"z", kCmd},
      {MenuRole::kRedo, @selector(redo:), @"Redo", @"z", kCmd | NSEventModifierFlagShift},
      {MenuRole::kCut, @selector(cut:), @"Cut", @"x", kCmd},
      {MenuRole::kCopy, @selector(copy:), @"Copy", @"c", kCmd},
      {MenuRole::kPaste, @selector(paste:), @"Paste", @"v", kCmd},
      {MenuRole::kSelectAll, @selector(selectAll:), @"Select All", @"a", kCmd},
      {MenuRole::kMinimize, @selector(performMiniaturize:), @"Minimize", @"m", kCmd},
      {MenuRole::kZoom, @selector(performZoom:), @"Zoom", @"", 0},
      {MenuRole::kClose, @selector(performClose:), @"Close Window", @"w", kCmd},
      {MenuRole::kFront, @selector(arrangeInFront:), @"Bring All to Front", @"", 0},
      {MenuRole::kAppMenu, nullptr, @"%@", @"", 0},
      {MenuRole::kEditMenu, nullptr, @"Edit", @"", 0},
      {MenuRole::kWindowMenu, nullptr, @"Window", @"", 0},
  };
  for (const RoleInfo& info : kRoles) {
    if (info.role == role) return &info;
  }
  return nullptr;
}

// MenuAccelerator.key → key equivalent(一个字符)。
NSString* KeyEquivalent(const std::string& key) {
  static const struct {
    std::string_view name;
    unichar character;
  } kNamed[] = {
      {"Enter", NSCarriageReturnCharacter}, {"Tab", NSTabCharacter},          {"Space", ' '},
      {"Backspace", NSBackspaceCharacter},  {"Delete", NSDeleteFunctionKey},  {"Insert", NSInsertFunctionKey},
      {"Escape", 0x1B},                     {"Up", NSUpArrowFunctionKey},     {"Down", NSDownArrowFunctionKey},
      {"Left", NSLeftArrowFunctionKey},     {"Right", NSRightArrowFunctionKey}, {"Home", NSHomeFunctionKey},
      {"End", NSEndFunctionKey},            {"PageUp", NSPageUpFunctionKey},  {"PageDown", NSPageDownFunctionKey},
  };
  for (const auto& named : kNamed) {
    if (named.name == key) return [NSString stringWithCharacters:&named.character length:1];
  }
  if (key.size() >= 2 && key[0] == 'F') {  // F1–F24
    const int n = std::atoi(key.c_str() + 1);
    if (n >= 1 && n <= 24) {
      const unichar c = static_cast<unichar>(NSF1FunctionKey + n - 1);
      return [NSString stringWithCharacters:&c length:1];
    }
  }
  return ToNSString(key);  // 单个字符(小写;Shift 由修饰键位表示)
}

NSEventModifierFlags ModifierFlags(uint32_t modifiers) {
  NSEventModifierFlags flags = 0;
  if ((modifiers & platform::kModifierShift) != 0) flags |= NSEventModifierFlagShift;
  if ((modifiers & platform::kModifierControl) != 0) flags |= NSEventModifierFlagControl;
  if ((modifiers & platform::kModifierAlt) != 0) flags |= NSEventModifierFlagOption;
  if ((modifiers & platform::kModifierMeta) != 0) flags |= NSEventModifierFlagCommand;
  return flags;
}

// 没有设置应用菜单时的默认菜单: 只有应用菜单(隐藏、退出)。
MenuModel DefaultApplicationMenu() {
  MenuItem app{.type = MenuItemType::kSubmenu, .role = MenuRole::kAppMenu};
  app.submenu.push_back({.role = MenuRole::kHide});
  app.submenu.push_back({.type = MenuItemType::kSeparator});
  app.submenu.push_back({.role = MenuRole::kQuit});
  return {std::move(app)};
}

}  // namespace

MacMenus::MacMenus() : target_([[MomoMenuTarget alloc] init]) {
  NSApp.mainMenu = [[NSMenu alloc] initWithTitle:@""];
  SetApplicationMenu(nullptr);
}

MacMenus::~MacMenus() { target_.delegate = nullptr; }

void MacMenus::SetDelegate(MenuDelegate* delegate) { target_.delegate = delegate; }

void MacMenus::ConfigureItem(NSMenuItem* ns_item, const MenuItem& item) {
  const RoleInfo* role = FindRole(item.role);
  NSString* title = !item.label.empty() ? ToNSString(item.label)
                    : role != nullptr   ? [role->title stringByReplacingOccurrencesOfString:@"%@" withString:AppName()]
                                        : @"";
  ns_item.title = title;
  ns_item.tag = item.id;
  ns_item.state = item.checked ? NSControlStateValueOn : NSControlStateValueOff;
  if (item.accelerator.has_value()) {
    ns_item.keyEquivalent = KeyEquivalent(item.accelerator->key);
    ns_item.keyEquivalentModifierMask = ModifierFlags(item.accelerator->modifiers);
  } else {
    ns_item.keyEquivalent = role != nullptr ? role->key : @"";
    ns_item.keyEquivalentModifierMask = role != nullptr ? role->modifiers : 0;
  }
  if (item.type == MenuItemType::kSubmenu) {
    ns_item.submenu.title = title;
    UpdateMenu(ns_item.submenu, item.submenu);
    ns_item.enabled = item.enabled;
    // 这两个菜单的内容由 AppKit 维护(服务列表、窗口列表)。
    if (item.role == MenuRole::kServices) NSApp.servicesMenu = ns_item.submenu;
    if (item.role == MenuRole::kWindowMenu) NSApp.windowsMenu = ns_item.submenu;
    return;
  }
  // 菜单自动判定可用状态(autoenablesItems): 没有 action 的菜单项不可用;role 的 action 沿响应链找得到
  // 响应者才可用;普通菜单项的 target 总能响应。
  if (!item.enabled) {
    ns_item.action = nil;
    ns_item.target = nil;
  } else if (role != nullptr) {
    ns_item.action = role->action;
    ns_item.target = nil;
  } else {
    ns_item.action = @selector(itemClicked:);
    ns_item.target = target_;
  }
}

void MacMenus::UpdateMenu(NSMenu* menu, const MenuModel& model) {
  // 结构(项数、分隔线与子菜单的位置)没变时原地更新: 打开着的菜单不会被关掉。
  auto same_kind = [](NSMenuItem* ns_item, const MenuItem& item) {
    return ns_item.separatorItem == (item.type == MenuItemType::kSeparator) &&
           ns_item.hasSubmenu == (item.type == MenuItemType::kSubmenu);
  };
  bool same_structure = static_cast<size_t>(menu.numberOfItems) == model.size();
  for (size_t i = 0; same_structure && i < model.size(); ++i) {
    same_structure = same_kind([menu itemAtIndex:static_cast<NSInteger>(i)], model[i]);
  }
  if (!same_structure) {
    [menu removeAllItems];
    for (const MenuItem& item : model) {
      if (item.type == MenuItemType::kSeparator) {
        [menu addItem:[NSMenuItem separatorItem]];
        continue;
      }
      NSMenuItem* ns_item = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
      if (item.type == MenuItemType::kSubmenu) ns_item.submenu = [[NSMenu alloc] initWithTitle:@""];
      [menu addItem:ns_item];
    }
  }
  for (size_t i = 0; i < model.size(); ++i) {
    if (model[i].type != MenuItemType::kSeparator) {
      ConfigureItem([menu itemAtIndex:static_cast<NSInteger>(i)], model[i]);
    }
  }
}

NSMenu* MacMenus::BuildMenu(const MenuModel& model) {
  NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
  UpdateMenu(menu, model);
  return menu;
}

void MacMenus::SetApplicationMenu(const MenuModel* menu) {
  // 上一份菜单里的服务菜单 / 窗口菜单不必清除(AppKit 也不接受置空): 新菜单里有就被替换, 没有时
  // AppKit 继续维护的是已不在菜单栏里的旧菜单, 没有可见效果。
  UpdateMenu(NSApp.mainMenu, menu != nullptr && !menu->empty() ? *menu : DefaultApplicationMenu());
}

void MacMenus::Popup(const MenuModel& model, PlatformWindow* window, std::optional<Point> position,
                     MoveOnlyFunction<void()> on_closed) {
  ClosePopup();
  NSMenu* menu = BuildMenu(model);
  popup_ = menu;
  NSView* view = window != nullptr ? (__bridge NSView*)window->native_handle() : nil;
  auto closed = std::make_shared<MoveOnlyFunction<void()>>(std::move(on_closed));
  // 菜单的跟踪循环是嵌套的 run loop: 从 run loop 的 block 进入(而不是 GCD 主队列的任务), 期间主队列——
  // 也就是 platform EventLoop——照常运转。
  CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
    if (popup_ == menu) {  // 尚未被 ClosePopup / 下一次 Popup 取消
      if (view.window != nil && position.has_value()) {
        const CGFloat y = view.isFlipped ? position->y : NSHeight(view.bounds) - position->y;
        [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(position->x, y) inView:view];
      } else {
        [menu popUpMenuPositioningItem:nil atLocation:NSEvent.mouseLocation inView:nil];
      }
      if (popup_ == menu) popup_ = nil;
    }
    (*closed)();
  });
  CFRunLoopWakeUp(CFRunLoopGetMain());
}

void MacMenus::ClosePopup() {
  NSMenu* menu = std::exchange(popup_, nil);
  [menu cancelTrackingWithoutAnimation];  // 尚未弹出时无效果: block 看到 popup_ 已变, 不再弹出
}

StatusOr<std::unique_ptr<PlatformTray>> MacMenus::CreateTray(int64_t tray_id, const std::string& image_path,
                                                             TrayDelegate* delegate) {
  auto tray = std::make_unique<MacTray>(*this, tray_id, delegate);
  MOMO_RETURN_IF_ERROR(tray->SetImage(image_path));
  return std::unique_ptr<PlatformTray>(std::move(tray));
}

}  // namespace momo::embedder::macos
