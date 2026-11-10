#include "embedder/macos/mac_window.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "base/logging.h"
#include "embedder/macos/mac_key_mapping.h"
#include "embedder/macos/mac_mouse_dispatcher.h"
#include "embedder/macos/mac_platform_views.h"
#include "embedder/macos/mac_surface_metal.h"
#include "embedder/macos/mac_surface_software.h"
#include "embedder/macos/mac_text_input.h"

using momo::engine::platform::KeyData;
using momo::engine::platform::KeyEventType;
using momo::engine::platform::PointerData;
using momo::engine::platform::PointerDataPacket;
using momo::engine::platform::PointerDeviceKind;
using momo::engine::platform::PointerPhase;
using momo::embedder::macos::MacMetalSurface;
using momo::embedder::macos::MacTextInput;
using momo::embedder::macos::MacWindow;

namespace {

// 非精细滚动(普通滚轮)每行对应的逻辑像素(与 Chromium 一致)。
constexpr double kScrollLinePixels = 40.0;

int64_t TimestampUs(NSEvent* event) { return static_cast<int64_t>(event.timestamp * 1e6); }

uint32_t ButtonBit(NSEvent* event) {
  switch (event.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
    case NSEventTypeLeftMouseDragged:
      return 1u << 0;
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
    case NSEventTypeRightMouseDragged:
      return 1u << 1;
    default: {
      // 其他按钮: buttonNumber 2 = 中键(bit2), 3/4 = 侧键 …
      const NSInteger number = event.buttonNumber;
      return number >= 2 && number < 32 ? (1u << number) : (1u << 2);
    }
  }
}

uint32_t ToModifiers(NSEventModifierFlags flags) {
  uint32_t modifiers = momo::engine::platform::kModifierNone;
  if (flags & NSEventModifierFlagShift) modifiers |= momo::engine::platform::kModifierShift;
  if (flags & NSEventModifierFlagControl) modifiers |= momo::engine::platform::kModifierControl;
  if (flags & NSEventModifierFlagOption) modifiers |= momo::engine::platform::kModifierAlt;
  if (flags & NSEventModifierFlagCommand) modifiers |= momo::engine::platform::kModifierMeta;
  if (flags & NSEventModifierFlagCapsLock) modifiers |= momo::engine::platform::kModifierCapsLock;
  return modifiers;
}

// 字符串的第一个 Unicode 码点(处理代理对);空串返回 0。
uint32_t FirstCodePoint(NSString* string) {
  if (string.length == 0) {
    return 0;
  }
  const unichar high = [string characterAtIndex:0];
  if (CFStringIsSurrogateHighCharacter(high) && string.length > 1) {
    const unichar low = [string characterAtIndex:1];
    if (CFStringIsSurrogateLowCharacter(low)) {
      return CFStringGetLongCharacterForSurrogatePair(high, low);
    }
  }
  return high;
}

// 控制字符与 AppKit 功能键私有区(NSUpArrowFunctionKey 等 0xF700–0xF8FF)不是"可打印字符"。
bool IsPrintable(uint32_t code_point) {
  return code_point >= 0x20 && code_point != 0x7F && !(code_point >= 0xF700 && code_point <= 0xF8FF);
}

bool AllPrintable(NSString* string) {
  for (NSUInteger i = 0; i < string.length; ++i) {
    const unichar c = [string characterAtIndex:i];
    if (c < 0x20 || c == 0x7F || (c >= 0xF700 && c <= 0xF8FF)) {
      return false;
    }
  }
  return string.length > 0;
}

uint64_t SpecialLogicalKey(uint16_t key_code, uint32_t usage) {
  // 无 HID usage 的键: 仍给出唯一值(放在 usage 空间之外)。
  return momo::embedder::macos::kSpecialKeyBase + (usage != 0 ? usage : (0x10000u + key_code));
}

uint64_t LogicalKeyFor(NSEvent* event, uint32_t usage) {
  const uint16_t key_code = event.keyCode;
  if (momo::embedder::macos::IsSpecialKeyCode(key_code)) {
    return SpecialLogicalKey(key_code, usage);
  }
  // 当前键盘布局下不带修饰键时的字符(Shift+1 → "1", Option+e → "e"), 小写。
  NSString* base = [event charactersByApplyingModifiers:0];
  if (base.length == 0) {
    base = event.charactersIgnoringModifiers;  // 死键等
  }
  const uint32_t code_point = FirstCodePoint(base.lowercaseString);
  if (!IsPrintable(code_point)) {
    return SpecialLogicalKey(key_code, usage);
  }
  return code_point;
}

}  // namespace

// ---------------------------------------------------------------------------
// MomoContentView: 输入归一化 + NSTextInputClient + 光标。

@interface MomoContentView : NSView <NSTextInputClient, MomoMouseEventTarget>
- (void)attachOwner:(MacWindow*)owner;
- (void)detachOwner;
- (void)setMomoCursor:(NSCursor*)cursor hidden:(BOOL)hidden;
@end

@implementation MomoContentView {
  MacWindow* _owner;             // 非拥有;MacWindow 析构时清空
  NSTrackingArea* _trackingArea;
  id _keyUpMonitor;
  NSEvent* __weak _lastKeyUp;    // 最近处理过的 keyUp(去重: 监视器补发后 AppKit 若也投递, 只处理一次)
  uint32_t _buttons;             // 当前按下的按钮位图(逻辑: Control+左键记为次键)
  BOOL _controlClick;            // 当前左键按下是 Control+单击(macOS 惯例: 视为右键)
  BOOL _pointerAdded;            // 已发出 kAdd(尚未 kRemove)
  BOOL _inside;                  // 指针位于视图内
  NSCursor* _cursor;
  BOOL _wantCursorHidden;
  BOOL _cursorHideApplied;
}

- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    _cursor = [NSCursor arrowCursor];
    const NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited |
                                          NSTrackingMouseMoved | NSTrackingCursorUpdate |
                                          NSTrackingActiveAlways | NSTrackingInVisibleRect |
                                          NSTrackingEnabledDuringMouseDrag;
    _trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                 options:options
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
  }
  return self;
}

- (void)attachOwner:(MacWindow*)owner {
  _owner = owner;
  // AppKit 在 Command 按下时, 由 -[NSApplication sendEvent:] 吞掉 keyUp, 不发给窗口与 first responder
  // (长期存在的已知行为)。用本地监视器补发, 与 Flutter macOS embedder 的做法相同;
  // 子类化 NSApplication 也能解决, 但要改 NSPrincipalClass, 更侵入。重复投递由 keyUp: 去重。
  MomoContentView* __weak weakSelf = self;
  _keyUpMonitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskKeyUp
                                   handler:^NSEvent*(NSEvent* event) {
                                     MomoContentView* view = weakSelf;
                                     if (view != nil && event.window == view.window &&
                                         view.window.firstResponder == view &&
                                         (event.modifierFlags & NSEventModifierFlagCommand)) {
                                       [view keyUp:event];
                                     }
                                     return event;
                                   }];
}

- (void)detachOwner {
  _owner = nullptr;
  if (_keyUpMonitor != nil) {
    [NSEvent removeMonitor:_keyUpMonitor];
    _keyUpMonitor = nil;
  }
  if (_cursorHideApplied) {
    [NSCursor unhide];
    _cursorHideApplied = NO;
  }
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

// 焦点离开内容视图: 若去了某个平台视图, 通知引擎(框架据此让出 momo 焦点, 收起输入法连接)。
- (BOOL)resignFirstResponder {
  const BOOL resigned = [super resignFirstResponder];
  if (resigned && _owner != nullptr) {
    MomoContentView* __weak weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      MomoContentView* view = weakSelf;
      if (view == nil || view->_owner == nullptr || view.window == nil) {
        return;
      }
      MacWindow* owner = view->_owner;
      const int64_t view_id =
          [owner->platform_views() viewIdForResponder:view.window.firstResponder];
      if (view_id != 0) {
        owner->delegate()->OnPlatformViewFocused(view_id);
      }
    });
  }
  return resigned;
}
- (BOOL)isOpaque {
  return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return YES;
}

// ---- 几何与 surface ----

- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  if (_owner != nullptr && _owner->metal_surface() != nullptr) {
    _owner->metal_surface()->UpdateGeometry();
  }
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (_owner != nullptr) {
    if (_owner->metal_surface() != nullptr) {
      _owner->metal_surface()->UpdateGeometry();
    }
    _owner->NotifyMetricsChanged();
  }
}

// ---- 指针 ----

- (double)scale {
  const CGFloat scale = self.window.backingScaleFactor;
  return scale > 0 ? scale : 1.0;
}

- (PointerData)pointerFromEvent:(NSEvent*)event phase:(PointerPhase)phase {
  PointerData data;
  data.timestamp_us = TimestampUs(event);
  data.kind = PointerDeviceKind::kMouse;
  data.phase = phase;
  data.device = 0;
  // 视图坐标(点, 左下原点)→ 物理像素(左上原点)。
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  const double scale = [self scale];
  data.physical_x = p.x * scale;
  data.physical_y = (self.bounds.size.height - p.y) * scale;
  data.buttons = _buttons;
  return data;
}

- (void)dispatchPointer:(PointerData)data {
  if (_owner == nullptr) {
    return;
  }
  PointerDataPacket packet;
  if (data.phase == PointerPhase::kRemove) {
    if (!_pointerAdded) {
      return;
    }
    _pointerAdded = NO;
  } else if (!_pointerAdded) {
    // 首次出现(进入视图或首个事件): 先补一个 kAdd。
    PointerData add = data;
    add.phase = PointerPhase::kAdd;
    add.kind = PointerDeviceKind::kMouse;
    add.buttons = 0;
    add.scroll_delta_x = add.scroll_delta_y = 0;
    add.momentum = false;
    packet.push_back(add);
    _pointerAdded = YES;
  }
  packet.push_back(data);
  _owner->delegate()->OnPointerPacket(std::move(packet));
}

- (BOOL)mouseIsInside {
  const NSPoint p = [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil];
  return NSPointInRect(p, self.bounds);
}

// 逻辑按钮位: Control+左键按下视为次键(与 AppKit 弹出上下文菜单的惯例一致), 直到该左键抬起。
- (uint32_t)logicalButtonBit:(NSEvent*)event {
  const uint32_t bit = ButtonBit(event);
  return bit == 1u && _controlClick ? (1u << 1) : bit;
}

- (void)handleButtonDown:(NSEvent*)event {
  // 点击 momo 区域: 若键盘焦点在某个平台视图里, 取回(否则键盘事件仍发往原生视图)。
  if (self.window != nil && self.window.firstResponder != self) {
    [self.window makeFirstResponder:self];
  }
  // 之前的按下没有收到抬起(被原生菜单等嵌套事件循环吞掉): 先补一个抬起, 复位框架侧的指针状态。
  if (_buttons != 0 && ([NSEvent pressedMouseButtons] & ~(NSUInteger)ButtonBit(event)) == 0) {
    _buttons = 0;
    _controlClick = NO;
    [self dispatchPointer:[self pointerFromEvent:event phase:PointerPhase::kUp]];
  }
  if (event.type == NSEventTypeLeftMouseDown) {
    _controlClick = (event.modifierFlags & NSEventModifierFlagControl) != 0;
  }
  const BOOL first = _buttons == 0;
  _buttons |= [self logicalButtonBit:event];
  [self dispatchPointer:[self pointerFromEvent:event
                                         phase:first ? PointerPhase::kDown : PointerPhase::kMove]];
}

- (void)handleButtonUp:(NSEvent*)event {
  _buttons &= ~[self logicalButtonBit:event];
  if (event.type == NSEventTypeLeftMouseUp) {
    _controlClick = NO;
  }
  const BOOL last = _buttons == 0;
  [self dispatchPointer:[self pointerFromEvent:event
                                         phase:last ? PointerPhase::kUp : PointerPhase::kMove]];
  if (last && ![self mouseIsInside]) {
    // 拖出视图后松开: 补发离开。
    _inside = NO;
    [self dispatchPointer:[self pointerFromEvent:event phase:PointerPhase::kRemove]];
  }
}

- (void)handleMove:(NSEvent*)event {
  _inside = YES;
  [self dispatchPointer:[self pointerFromEvent:event
                                         phase:_buttons == 0 ? PointerPhase::kHover
                                                             : PointerPhase::kMove]];
}

- (void)handleScroll:(NSEvent*)event {
  PointerData data = [self pointerFromEvent:event phase:PointerPhase::kScroll];
  const double scale = [self scale];
  double dx = event.scrollingDeltaX;
  double dy = event.scrollingDeltaY;
  if (event.hasPreciseScrollingDeltas) {
    // 触控板 / Magic Mouse: 点 → 物理像素。
    data.kind = PointerDeviceKind::kTrackpad;
    dx *= scale;
    dy *= scale;
  } else {
    // 普通滚轮: 行 → 像素。
    dx *= kScrollLinePixels * scale;
    dy *= kScrollLinePixels * scale;
  }
  // AppKit 的 delta 是"内容移动方向";统一为"视口滚动方向"(正 y = 向下滚, 与 Web deltaY 一致)。
  data.scroll_delta_x = -dx;
  data.scroll_delta_y = -dy;
  data.momentum = event.momentumPhase != NSEventPhaseNone;
  if (data.scroll_delta_x == 0 && data.scroll_delta_y == 0) {
    return;  // 手势开始/结束等零位移事件
  }
  [self dispatchPointer:data];
}

// ---- MomoMouseEventTarget(由 MomoWindow 的派发器调用, 位于全部平台视图之下)----
// 内容视图是最底层目标: 指针在视图内即消费。AppKit 经响应者方法投递来的鼠标事件一律忽略
// (见下方空实现), 保证 momo 只从派发器收到事件, 不会与平台视图重复接收。
- (BOOL)momoHandleMouseEvent:(NSEvent*)event captured:(BOOL)captured {
  if (_owner == nullptr) {
    return NO;
  }
  if (!captured) {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (!NSPointInRect(p, self.bounds)) {
      return NO;
    }
  }
  switch (MomoClassifyMouseEvent(event)) {
    case MomoMouseEventKindDown:
      [self handleButtonDown:event];
      break;
    case MomoMouseEventKindUp:
      [self handleButtonUp:event];
      break;
    case MomoMouseEventKindDrag:
    case MomoMouseEventKindMove:
    case MomoMouseEventKindEnter:
      [self handleMove:event];
      [self refreshCursor];
      break;
    case MomoMouseEventKindScroll:
      [self handleScroll:event];
      break;
    case MomoMouseEventKindExit:
    case MomoMouseEventKindNone:
      break;
  }
  return YES;
}

// 悬停移到平台视图上或离开窗口: 对 momo 而言指针离开(hover 复位);回来时 dispatchPointer 自动补 kAdd。
- (void)momoMouseLeft:(NSEvent*)event {
  _inside = NO;
  if (_buttons == 0) {
    [self dispatchPointer:[self pointerFromEvent:event phase:PointerPhase::kRemove]];
  }
  [self refreshCursor];
}

// AppKit 的响应者投递(含跟踪区域、响应链冒泡): 全部由派发器接管, 这里不处理也不再向上转发。
- (void)mouseDown:(NSEvent*)event {}
- (void)mouseUp:(NSEvent*)event {}
- (void)rightMouseDown:(NSEvent*)event {}
- (void)rightMouseUp:(NSEvent*)event {}
- (void)otherMouseDown:(NSEvent*)event {}
- (void)otherMouseUp:(NSEvent*)event {}
- (void)mouseDragged:(NSEvent*)event {}
- (void)rightMouseDragged:(NSEvent*)event {}
- (void)otherMouseDragged:(NSEvent*)event {}
- (void)mouseMoved:(NSEvent*)event {}
- (void)mouseEntered:(NSEvent*)event {}
- (void)mouseExited:(NSEvent*)event {}
- (void)scrollWheel:(NSEvent*)event {}

// ---- 光标 ----

- (void)setMomoCursor:(NSCursor*)cursor hidden:(BOOL)hidden {
  if (cursor != nil) {
    _cursor = cursor;
  }
  _wantCursorHidden = hidden;
  [self refreshCursor];
}

- (void)refreshCursor {
  BOOL inside = [self mouseIsInside];
  if (inside && _owner != nullptr && _owner->platform_views() != nil) {
    // 鼠标在平台视图上: 光标归原生视图管理(如 WKWebView 的 I-beam / 手形)。
    const NSPoint p = [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil];
    if ([_owner->platform_views() platformViewAtContentPoint:p]) {
      inside = NO;
    }
  }
  const BOOL hide = _wantCursorHidden && inside && _owner != nullptr;
  if (hide != _cursorHideApplied) {
    if (hide) {
      [NSCursor hide];
    } else {
      [NSCursor unhide];
    }
    _cursorHideApplied = hide;
  }
  if (inside && !hide) {
    [_cursor set];
  }
}

- (void)cursorUpdate:(NSEvent*)event {
  [self refreshCursor];
}

// ---- 键盘 ----

- (void)dispatchKey:(NSEvent*)event type:(KeyEventType)type {
  if (_owner == nullptr) {
    return;
  }
  const uint32_t usage = momo::embedder::macos::MacKeyCodeToHidUsage(event.keyCode);
  KeyData data;
  data.timestamp_us = TimestampUs(event);
  data.type = type;
  data.physical_key = momo::embedder::macos::PhysicalKeyFromHidUsage(usage);
  data.logical_key = LogicalKeyFor(event, usage);
  data.modifiers = ToModifiers(event.modifierFlags);
  if (type != KeyEventType::kUp) {
    NSString* characters = event.characters;
    if (AllPrintable(characters)) {
      data.character = characters.UTF8String ?: "";
    }
  }
  _owner->delegate()->OnKeyData(data);
}

- (void)keyDown:(NSEvent*)event {
  [self dispatchKey:event type:event.isARepeat ? KeyEventType::kRepeat : KeyEventType::kDown];
  // 文本输入会话期间同时交给输入法(insertText / setMarkedText / doCommandBySelector)。
  // 是否把同一按键既当字符又当快捷键, 由框架层判断。
  if (_owner != nullptr && _owner->mac_text_input().active()) {
    [self interpretKeyEvents:@[ event ]];
  }
}

- (void)keyUp:(NSEvent*)event {
  // 同一个 NSEvent 对象既可能经本地监视器补发, 也可能被 AppKit 正常投递: 只处理一次。
  if (event == _lastKeyUp) {
    return;
  }
  _lastKeyUp = event;
  [self dispatchKey:event type:KeyEventType::kUp];
}

- (void)flagsChanged:(NSEvent*)event {
  if (_owner == nullptr) {
    return;
  }
  const uint16_t key_code = event.keyCode;
  const uint32_t usage = momo::embedder::macos::MacKeyCodeToHidUsage(key_code);
  if (usage == 0) {
    return;  // Fn 等无 HID usage 的修饰键
  }
  KeyData data;
  data.timestamp_us = TimestampUs(event);
  data.physical_key = momo::embedder::macos::PhysicalKeyFromHidUsage(usage);
  data.logical_key = SpecialLogicalKey(key_code, usage);
  data.modifiers = ToModifiers(event.modifierFlags);
  // 注意: flagsChanged 事件不能访问 characters(会抛 ObjC 异常)。
  if (key_code == 0x39) {
    // CapsLock: 每次按下切换锁定状态, 只有一个事件;合成一次完整按键。
    data.type = KeyEventType::kDown;
    _owner->delegate()->OnKeyData(data);
    if (_owner == nullptr) {
      return;
    }
    data.type = KeyEventType::kUp;
    _owner->delegate()->OnKeyData(data);
    return;
  }
  NSEventModifierFlags class_flag = 0;
  switch (usage) {
    case 0xE0: case 0xE4: class_flag = NSEventModifierFlagControl; break;
    case 0xE1: case 0xE5: class_flag = NSEventModifierFlagShift; break;
    case 0xE2: case 0xE6: class_flag = NSEventModifierFlagOption; break;
    case 0xE3: case 0xE7: class_flag = NSEventModifierFlagCommand; break;
    default: return;
  }
  const uint64_t device_mask = momo::embedder::macos::DeviceModifierMaskForKeyCode(key_code);
  const bool down = (event.modifierFlags & class_flag) != 0 &&
                    (device_mask == 0 || (event.modifierFlags & device_mask) != 0);
  data.type = down ? KeyEventType::kDown : KeyEventType::kUp;
  _owner->delegate()->OnKeyData(data);
}

// ---- NSTextInputClient(委托 MacTextInput) ----

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
  if (_owner != nullptr) {
    _owner->mac_text_input().InsertText(string, replacementRange);
  }
}

- (void)doCommandBySelector:(SEL)selector {
  // 由按键派生的编辑命令(退格、方向键、回车……)不再上报: 同一按键已经以 KeyData 送达框架,
  // 框架的 EditableText 统一按键处理(跨平台一致);再发 kCommand 会导致执行两次。
  // kCommand 保留给没有对应按键事件的命令来源(见 text_input_connection.h)。
  (void)selector;
}

- (void)setMarkedText:(id)string
        selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange {
  if (_owner != nullptr) {
    _owner->mac_text_input().SetMarkedText(string, selectedRange, replacementRange);
  }
}

- (void)unmarkText {
  if (_owner != nullptr) {
    _owner->mac_text_input().UnmarkText();
  }
}

- (NSRange)selectedRange {
  return _owner != nullptr ? _owner->mac_text_input().SelectedRange() : NSMakeRange(NSNotFound, 0);
}

- (NSRange)markedRange {
  return _owner != nullptr ? _owner->mac_text_input().MarkedRange() : NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText {
  return _owner != nullptr && _owner->mac_text_input().HasMarkedText();
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range
                                               actualRange:(NSRangePointer)actualRange {
  return _owner != nullptr ? _owner->mac_text_input().AttributedSubstring(range, actualRange) : nil;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
  return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
  return _owner != nullptr
             ? _owner->mac_text_input().FirstRectForCharacterRange(range, actualRange)
             : NSZeroRect;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
  return NSNotFound;  // 命中测试由 JS 框架负责
}

@end

// ---------------------------------------------------------------------------

// MomoWindow: 鼠标事件的唯一入口(docs/05 §9 "输入")。所有鼠标事件先经 MomoMouseEventDispatcher
// 按 z 序选出唯一的消费者, 再决定是否交给 AppKit 投递:
//   - 消费者是平台视图(momoDeliversViaAppKit): 调 super, AppKit 命中测试期间宿主视图只返回该原生视图;
//   - 消费者是 momo 内容视图: 已在派发器中处理。按钮事件仍调 super(窗口激活、第一响应者、AppKit 自身的
//     按下跟踪), 此时命中落到内容视图, 其响应者方法为空实现;移动/滚动不调 super, 原生视图收不到;
//   - 原生视图自己跟踪区域的进入/离开事件照常交给 AppKit(只接管内容视图跟踪区域的边界事件)。
@interface MomoWindow : NSWindow
@property(nonatomic, strong) MomoMouseEventDispatcher* mouseDispatcher;
@property(nonatomic, weak) NSView* momoContentView;
// 可拖动窗口的区域(内容视图逻辑坐标, 左上原点;无边框窗口由应用声明)。
@property(nonatomic) std::vector<NSRect> dragRegions;
@end

@implementation MomoWindow {
  BOOL _systemTracking;  // 当前按下序列交给了 AppKit(红绿灯等窗口控件), 直到抬起
  BOOL _momoOnly;        // 当前按下序列落在透明标题栏区域内的 momo 内容上: 不交给 AppKit(否则会拖动窗口)
}

- (BOOL)inDragRegion:(NSEvent*)event {
  NSView* content = self.momoContentView;
  if (content == nil || _dragRegions.empty()) return NO;
  const NSPoint p = [content convertPoint:event.locationInWindow fromView:nil];
  const NSPoint top_left = NSMakePoint(p.x, content.bounds.size.height - p.y);
  for (const NSRect& r : _dragRegions) {
    if (NSPointInRect(top_left, r)) return YES;
  }
  return NO;
}

// 在 theme frame 上命中测试(隐藏标题栏后, 透明标题栏视图与红绿灯浮在内容之上)。
- (NSView*)frameHit:(NSEvent*)event {
  NSView* frame = self.momoContentView.superview;
  return frame != nil ? [frame hitTest:[frame convertPoint:event.locationInWindow fromView:nil]] : nil;
}

// 命中窗口控件按钮(红绿灯)。
- (BOOL)hitsWindowControl:(NSView*)hit {
  for (NSView* v = hit; v != nil; v = v.superview) {
    if ([v isKindOfClass:[NSButton class]]) return YES;
  }
  return NO;
}

// 命中内容视图之外的非按钮部分(透明标题栏视图): 这里的 momo 内容不能交给 AppKit, 否则按下即拖动窗口。
- (BOOL)hitsTitlebarChrome:(NSView*)hit {
  NSView* content = self.momoContentView;
  return hit != nil && content != nil && hit != content.superview && ![hit isDescendantOf:content];
}

- (void)titleBarDoubleClick {
  // 遵循系统设置"连按窗口标题栏以…"(缩放 / 最小化 / 无操作)。
  NSString* action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
  if ([action isEqualToString:@"Minimize"]) {
    [self miniaturize:nil];
  } else if (![action isEqualToString:@"None"]) {
    [self zoom:nil];
  }
}

- (void)sendEvent:(NSEvent*)event {
  MomoMouseEventDispatcher* dispatcher = self.mouseDispatcher;
  const MomoMouseEventKind kind = MomoClassifyMouseEvent(event);
  if (dispatcher == nil || kind == MomoMouseEventKindNone) {
    [super sendEvent:event];
    return;
  }
  // 窗口控件(红绿灯)与拖动区域优先于 momo: 整个按下序列交给 AppKit。
  if (_systemTracking) {
    [super sendEvent:event];
    if (kind == MomoMouseEventKindUp) _systemTracking = NO;
    return;
  }
  if (kind == MomoMouseEventKindDown && !_momoOnly) {
    NSView* hit = [self frameHit:event];
    if (event.type == NSEventTypeLeftMouseDown && [self hitsWindowControl:hit]) {
      _systemTracking = YES;
      [super sendEvent:event];
      return;
    }
    _momoOnly = [self hitsTitlebarChrome:hit];
  }
  if (event.type == NSEventTypeLeftMouseDown) {
    if ([self inDragRegion:event]) {
      _momoOnly = NO;  // 拖动/双击由 AppKit 处理, 不进入 momo
      if (event.clickCount == 2) {
        [self titleBarDoubleClick];
      } else {
        [self performWindowDragWithEvent:event];  // 拖动循环内消化抬起事件
      }
      return;
    }
  }
  if (kind == MomoMouseEventKindEnter || kind == MomoMouseEventKindExit) {
    if (event.trackingArea == nil || event.trackingArea.owner != self.momoContentView) {
      [super sendEvent:event];  // 原生视图自己的跟踪区域
      return;
    }
    [dispatcher dispatchEvent:event];
    return;
  }
  id<MomoMouseEventTarget> target = [dispatcher dispatchEvent:event];
  const BOOL via_appkit = target != nil &&
                          [target respondsToSelector:@selector(momoDeliversViaAppKit)] &&
                          [target momoDeliversViaAppKit];
  const BOOL button = kind == MomoMouseEventKindDown || kind == MomoMouseEventKindUp ||
                      kind == MomoMouseEventKindDrag;
  if (_momoOnly) {
    // 透明标题栏区域内的 momo 内容: 只由派发器处理;按下序列结束后恢复。
    if (kind == MomoMouseEventKindUp && dispatcher.pressedButtons == 0) _momoOnly = NO;
    return;
  }
  if (via_appkit || button || target == nil) {
    [dispatcher beginRoutingToTarget:target];
    [super sendEvent:event];
    [dispatcher endRouting];
  }
}
@end

@interface MomoWindowObserver : NSObject <NSWindowDelegate>
- (instancetype)initWithOwner:(MacWindow*)owner;
@end

@implementation MomoWindowObserver {
  MacWindow* _owner;
}
- (instancetype)initWithOwner:(MacWindow*)owner {
  if ((self = [super init])) {
    _owner = owner;
  }
  return self;
}
- (BOOL)windowShouldClose:(NSWindow*)sender {
  return _owner->delegate()->OnCloseRequested() ? YES : NO;
}
- (void)windowDidResize:(NSNotification*)notification {
  _owner->LayoutTrafficLights();
  _owner->NotifyMetricsChanged();
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
  _owner->LayoutTrafficLights();
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
  _owner->NotifyMetricsChanged();
}
- (void)windowDidChangeScreen:(NSNotification*)notification {
  _owner->NotifyMetricsChanged();
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
  _owner->delegate()->OnFocusChanged(true);
}
- (void)windowDidResignKey:(NSNotification*)notification {
  _owner->delegate()->OnFocusChanged(false);
}
- (void)windowDidMiniaturize:(NSNotification*)notification {
  _owner->delegate()->OnVisibilityChanged(false);
}
- (void)windowDidDeminiaturize:(NSNotification*)notification {
  _owner->delegate()->OnVisibilityChanged(true);
}
@end

namespace momo::embedder::macos {

namespace {

NSString* ToNSString(std::string_view utf8) {
  return [[NSString alloc] initWithBytes:utf8.data()
                                  length:utf8.size()
                                encoding:NSUTF8StringEncoding];
}

NSColor* ToNSColor(uint32_t argb) {
  return [NSColor colorWithSRGBRed:((argb >> 16) & 0xFF) / 255.0
                             green:((argb >> 8) & 0xFF) / 255.0
                              blue:(argb & 0xFF) / 255.0
                             alpha:((argb >> 24) & 0xFF) / 255.0];
}

bool SoftwareRenderingForced() {
  const char* value = std::getenv("MOMO_SOFTWARE_RENDERING");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

}  // namespace

MacWindow::MacWindow(const WindowSpec& spec, WindowDelegate* delegate) : delegate_(delegate) {
  NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable;
  if (spec.resizable) {
    style |= NSWindowStyleMaskResizable;
  }
  const NSRect frame = NSMakeRect(0, 0, spec.size.width, spec.size.height);
  MomoWindow* window = [[MomoWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
  window_ = window;
  window_.releasedWhenClosed = NO;  // 生命周期由本对象(ARC)管理
  window_.title = ToNSString(spec.title);
  window_.contentMinSize = NSMakeSize(spec.min_size.width, spec.min_size.height);
  window_.backgroundColor = ToNSColor(spec.background_argb);

  view_ = [[MomoContentView alloc] initWithFrame:frame];
  view_.wantsLayer = YES;
  view_.layer.backgroundColor = ToNSColor(spec.background_argb).CGColor;
  // 软件路径: 内容(IOSurface)铺满视图;尺寸与 backing 像素一致时为 1:1, 拖拽缩放期间先拉伸, 下一帧即修正。
  view_.layer.contentsGravity = kCAGravityResize;
  view_.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
  window_.contentView = view_;

  // Surface: Metal 优先;强制软件或无 Metal 设备时用 IOSurface 软件 surface。
  if (SoftwareRenderingForced()) {
    MOMO_LOG(INFO) << "MOMO_SOFTWARE_RENDERING set: using the software surface.";
  } else {
    StatusOr<std::unique_ptr<MacMetalSurface>> metal = MacMetalSurface::Create(view_);
    if (metal.ok()) {
      metal_surface_ = metal->get();
      surface_ = std::move(metal).value();
    } else {
      MOMO_LOG(WARNING) << "Metal surface unavailable (" << metal.status()
                        << "); using the software surface.";
    }
  }
  if (surface_ == nullptr) {
    surface_ = std::make_unique<MacSoftwareSurface>(view_);
  }

  // 平台视图宿主(只在 Metal surface 上支持合成;软件 surface 下视图保持隐藏)。
  platform_views_ = [[MomoPlatformViews alloc] initWithContentView:view_];
  if (metal_surface_ != nullptr) {
    metal_surface_->SetPlatformViews(platform_views_);
  }
  // 鼠标派发: 目标自上而下 = 可见平台视图(按 z 序)… → momo 内容视图(最底层, 兜底)。
  MomoMouseEventDispatcher* dispatcher = [[MomoMouseEventDispatcher alloc] init];
  MomoPlatformViews* __weak weak_views = platform_views_;
  MomoContentView* __weak weak_content = view_;
  dispatcher.targetsProvider = ^NSArray<id<MomoMouseEventTarget>>* {
    NSMutableArray<id<MomoMouseEventTarget>>* targets = [NSMutableArray array];
    MomoPlatformViews* views = weak_views;
    if (views != nil) {
      [targets addObjectsFromArray:[views mouseTargetsTopDown]];
    }
    MomoContentView* content = weak_content;
    if (content != nil) {
      [targets addObject:content];
    }
    return targets;
  };
  platform_views_.dispatcher = dispatcher;
  window.mouseDispatcher = dispatcher;
  window.momoContentView = view_;

  text_input_ = std::make_unique<MacTextInput>(view_, delegate_);
  [view_ attachOwner:this];
  [window_ makeFirstResponder:view_];

  observer_ = [[MomoWindowObserver alloc] initWithOwner:this];
  window_.delegate = observer_;
  [window_ center];
  last_metrics_ = metrics();
}

MacWindow::~MacWindow() {
  [view_ detachOwner];  // 之后的事件/IME 回调不再触达本对象
  window_.delegate = nil;
  [window_ orderOut:nil];
  [window_ close];
  surface_.reset();  // raster 已解绑(Shell::DetachView 先于窗口销毁)
  metal_surface_ = nullptr;
  platform_views_ = nil;  // 平台视图已由 Shell::DetachView 移出
  text_input_.reset();
  observer_ = nil;
  view_ = nil;
  window_ = nil;
}

void MacWindow::Show() {
  [window_ makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

void MacWindow::SetTitle(std::string_view title) { window_.title = ToNSString(title); }

void MacWindow::SetMinSize(Size logical_size) {
  window_.contentMinSize = NSMakeSize(logical_size.width, logical_size.height);
}

Size MacWindow::physical_size() const {
  const NSRect backing = [view_ convertRectToBacking:view_.bounds];
  return {backing.size.width, backing.size.height};
}

double MacWindow::device_pixel_ratio() const { return window_.backingScaleFactor; }

platform::RenderSurface& MacWindow::surface() { return *surface_; }

void MacWindow::SetCursor(platform::SystemCursor cursor) {
  NSCursor* ns_cursor = nil;
  switch (cursor) {
    case platform::SystemCursor::kArrow: ns_cursor = [NSCursor arrowCursor]; break;
    case platform::SystemCursor::kIBeam: ns_cursor = [NSCursor IBeamCursor]; break;
    case platform::SystemCursor::kPointingHand: ns_cursor = [NSCursor pointingHandCursor]; break;
    case platform::SystemCursor::kCrosshair: ns_cursor = [NSCursor crosshairCursor]; break;
    case platform::SystemCursor::kResizeLeftRight: ns_cursor = [NSCursor resizeLeftRightCursor]; break;
    case platform::SystemCursor::kResizeUpDown: ns_cursor = [NSCursor resizeUpDownCursor]; break;
    case platform::SystemCursor::kNotAllowed: ns_cursor = [NSCursor operationNotAllowedCursor]; break;
    case platform::SystemCursor::kHidden: break;
  }
  [view_ setMomoCursor:ns_cursor hidden:cursor == platform::SystemCursor::kHidden];
}

platform::TextInputConnection& MacWindow::text_input() { return *text_input_; }

void* MacWindow::native_handle() const { return (__bridge void*)view_; }

void MacWindow::SetAlwaysOnTop(bool on) {
  window_.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel;
  // 置顶窗口跟随用户切换到的桌面空间, 并可浮在全屏应用之上。
  window_.collectionBehavior = on ? (NSWindowCollectionBehaviorCanJoinAllSpaces |
                                     NSWindowCollectionBehaviorFullScreenAuxiliary)
                                  : NSWindowCollectionBehaviorDefault;
}

void MacWindow::SetTitleBar(bool hidden, double traffic_center_y) {
  titlebar_hidden_ = hidden;
  traffic_center_y_ = traffic_center_y;
  if (hidden) {
    window_.styleMask |= NSWindowStyleMaskFullSizeContentView;
    window_.titlebarAppearsTransparent = YES;
    window_.titleVisibility = NSWindowTitleHidden;
  } else {
    window_.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
    window_.titlebarAppearsTransparent = NO;
    window_.titleVisibility = NSWindowTitleVisible;
  }
  LayoutTrafficLights();
  NotifyMetricsChanged();  // 内容区包含/不含标题栏后尺寸变化
}

void MacWindow::LayoutTrafficLights() {
  if (!titlebar_hidden_ || traffic_center_y_ <= 0 ||
      (window_.styleMask & NSWindowStyleMaskFullScreen) != 0) {
    return;
  }
  NSButton* close = [window_ standardWindowButton:NSWindowCloseButton];
  NSView* titlebar = close.superview;         // NSTitlebarView
  NSView* container = titlebar.superview;     // NSTitlebarContainerView
  if (close == nil || container == nil) {
    return;
  }
  // 把标题栏容器的高度设为 2 × 中心距, 按钮在其中垂直居中。
  const CGFloat height = static_cast<CGFloat>(traffic_center_y_ * 2);
  NSRect container_frame = container.frame;
  container_frame.size.height = height;
  container_frame.origin.y = NSHeight(window_.frame) - height;
  container.frame = container_frame;
  titlebar.frame = container.bounds;
  for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
    NSButton* button = [window_ standardWindowButton:kind];
    NSRect f = button.frame;
    f.origin.y = (height - f.size.height) / 2;
    button.frame = f;
  }
}

void MacWindow::SetDragRegions(const std::vector<Rect>& rects) {
  std::vector<NSRect> regions;
  regions.reserve(rects.size());
  for (const Rect& r : rects) {
    regions.push_back(NSMakeRect(r.x, r.y, r.width, r.height));
  }
  ((MomoWindow*)window_).dragRegions = std::move(regions);
}

void MacWindow::Activate() {
  if (window_.miniaturized) {
    [window_ deminiaturize:nil];
  }
  [window_ makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

void MacWindow::AddPlatformView(int64_t view_id, void* native_view) {
  id object = (__bridge id)native_view;
  if (![object isKindOfClass:[NSView class]]) {
    MOMO_LOG(ERROR) << "Platform view " << view_id << " is not an NSView; ignored.";
    return;
  }
  [platform_views_ addView:(NSView*)object viewId:view_id];
}

void MacWindow::RemovePlatformView(int64_t view_id) { [platform_views_ removeViewId:view_id]; }

void MacWindow::FocusPlatformView(int64_t view_id) { [platform_views_ focusViewId:view_id]; }

void MacWindow::NotifyMetricsChanged() {
  const platform::ViewportMetrics current = metrics();
  if (current == last_metrics_) {
    return;
  }
  last_metrics_ = current;
  delegate_->OnMetricsChanged(current);
}

}  // namespace momo::embedder::macos
