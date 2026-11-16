#import "embedder/macos/mac_tray.h"

#import "embedder/macos/mac_menu.h"
#include "embedder/macos/mac_mouse_events.h"

@interface MomoTrayTarget : NSObject
@property(nonatomic, assign) momo::embedder::macos::MacTray* tray;  // 非拥有;MacTray 析构时清空
- (void)clicked:(id)sender;
@end

@implementation MomoTrayTarget
- (void)clicked:(id)sender {
  if (self.tray != nullptr) {
    self.tray->OnButtonAction();
  }
}
@end

namespace momo::embedder::macos {

namespace {

NSString* ToNSString(std::string_view utf8) {
  NSString* s = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
  return s ?: @"";
}

}  // namespace

MacTray::MacTray(MacMenus& menus, int64_t id, TrayDelegate* delegate)
    : menus_(menus),
      id_(id),
      delegate_(delegate),
      item_([NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength]),
      target_([[MomoTrayTarget alloc] init]) {
  target_.tray = this;
  item_.button.target = target_;
  item_.button.action = @selector(clicked:);
  [item_.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
}

MacTray::~MacTray() {
  target_.tray = nullptr;
  [NSStatusBar.systemStatusBar removeStatusItem:item_];
}

Status MacTray::SetImage(const std::string& path) {
  NSString* file = ToNSString(path);
  NSImage* image = [[NSImage alloc] initWithContentsOfFile:file];
  if (image == nil || !image.valid) {
    return InvalidArgumentError("cannot load the tray image '" + path + "'");
  }
  NSString* name = file.stringByDeletingPathExtension;
  // 高分屏表示: <name>@2x.<ext>(像素是 1x 的两倍, 逻辑尺寸相同)。
  NSString* retina = [[name stringByAppendingString:@"@2x"] stringByAppendingPathExtension:file.pathExtension];
  if (NSImageRep* rep = [NSImageRep imageRepWithContentsOfFile:retina]) {
    rep.size = image.size;
    [image addRepresentation:rep];
  }
  [image setTemplate:[name hasSuffix:@"Template"]];  // ("template" 是 C++ 关键字, 不能用点语法)
  item_.button.image = image;
  return Status::Ok();
}

void MacTray::SetToolTip(std::string_view text) { item_.button.toolTip = ToNSString(text); }

Status MacTray::SetTitle(std::string_view title) {
  item_.button.title = ToNSString(title);
  item_.button.imagePosition = title.empty() ? NSImageOnly : NSImageLeft;
  return Status::Ok();
}

void MacTray::SetContextMenu(const MenuModel* menu) {
  if (menu == nullptr) {
    item_.menu = nil;
  } else if (item_.menu != nil) {
    menus_.UpdateMenu(item_.menu, *menu);
  } else {
    item_.menu = menus_.BuildMenu(*menu);
  }
}

std::optional<Rect> MacTray::bounds() const {
  NSStatusBarButton* button = item_.button;
  if (button.window == nil || NSIsEmptyRect(button.window.frame)) {
    return std::nullopt;  // 尚未放进菜单栏
  }
  return ScreenRectFromCocoa([button.window convertRectToScreen:[button convertRect:button.bounds toView:nil]]);
}

void MacTray::OnButtonAction() {
  NSEvent* event = NSApp.currentEvent;
  const bool right = event.type == NSEventTypeRightMouseUp ||
                     (event.modifierFlags & NSEventModifierFlagControl) != 0;  // Control+点击 = 次键
  const TrayClick click = right                   ? TrayClick::kRightClick
                          : event.clickCount >= 2 ? TrayClick::kDoubleClick
                                                  : TrayClick::kClick;
  delegate_->OnTrayClicked(id_, click, bounds());
}

}  // namespace momo::embedder::macos
