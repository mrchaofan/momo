#include "embedder/macos/mac_window.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "base/event_loop.h"
#include "base/logging.h"
#include "embedder/macos/mac_key_mapping.h"
#include "embedder/macos/mac_mouse_events.h"
#include "embedder/macos/mac_platform_view_input.h"
#include "embedder/macos/mac_platform_views.h"
#include "embedder/macos/mac_surface_metal.h"
#include "embedder/macos/mac_surface_software.h"
#include "embedder/macos/mac_text_input.h"

using momo::engine::platform::KeyData;
using momo::engine::platform::KeyEventType;
using momo::engine::platform::PointerData;
using momo::engine::platform::PointerDataPacket;
using momo::engine::platform::PointerDeviceKind;
using momo::engine::platform::PlatformViewPointerAction;
using momo::engine::platform::PointerPhase;
using momo::engine::platform::PointerGestureKind;
using momo::engine::platform::SignalPhase;
using momo::embedder::macos::MacMetalSurface;
using momo::embedder::macos::MacTextInput;
using momo::embedder::macos::MacWindow;

namespace {

// 非精细滚动(普通滚轮)每行对应的逻辑像素(与 Chromium 一致)。
constexpr double kScrollLinePixels = 40.0;

int64_t TimestampUs(NSEvent* event) { return static_cast<int64_t>(event.timestamp * 1e6); }

// 屏幕坐标: Cocoa 以主屏左下为原点、y 向上;对外(PointerData::screen_x/y、screen_frame)统一为
// 主屏左上原点、y 向下的逻辑点(与 Quartz 全局坐标一致)。两者互换只需按主屏顶边翻转 y。
CGFloat PrimaryScreenTop() {
  NSScreen* primary = NSScreen.screens.firstObject;
  return primary != nil ? NSMaxY(primary.frame) : 0;
}
NSPoint FlipScreenY(NSPoint p) { return NSMakePoint(p.x, PrimaryScreenTop() - p.y); }

// 事件的屏幕位置(主屏左上原点): 取事件自带的全局位置。
// 不能用 [window convertPointToScreen:locationInWindow]: locationInWindow 相对事件产生时的窗口位置,
// 换算却用窗口当前位置——拖动中窗口跟随指针移动(setPosition)时两者不一致, 误差逐次放大, 窗口"脱手"。
NSPoint ScreenLocation(NSEvent* event) {
  CGEventRef cg = event.CGEvent;
  if (cg != nullptr) {
    const CGPoint p = CGEventGetLocation(cg);
    return NSMakePoint(p.x, p.y);
  }
  return FlipScreenY(NSEvent.mouseLocation);  // 合成事件: 退回当前指针位置
}

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

// MomoWindow(NSWindow)与 MomoPanel(NSPanel)共有的接口, 实现见下方。
@protocol MomoWindowLike <NSObject>
@property(nonatomic, weak) NSView* momoContentView;
// 可拖动窗口的区域(内容视图逻辑坐标, 左上原点;无边框窗口由应用声明)。
@property(nonatomic) std::vector<NSRect> dragRegions;
// 能否成为 main window。面板为 NO;始终可以成为 key window。
@property(nonatomic) BOOL canBecomeMain;
// 平台视图的输入路由: 回送事件的投递, 原生视图跟踪区事件的认领。
@property(nonatomic, weak) MomoPlatformViewInputRouter* inputRouter;
@end

@interface MomoContentView : NSView <NSTextInputClient>
// 窗口内全部鼠标事件的入口(MomoWindow -sendEvent:): 归一化为指针包交给 momo, 附带原生事件 id。
- (void)handleMouseEvent:(NSEvent*)event;
// 框架对原生事件的处置(view.dispatchPlatformViewPointer, docs/05 §9 "输入")。
- (void)platformViewPointer:(int64_t)viewId
              nativeEventId:(uint64_t)eventId
                     action:(PlatformViewPointerAction)action;
// 当前是否有按钮按下(本窗口的按下序列进行中)。
@property(nonatomic, readonly) BOOL hasPressedButtons;
// 事件位于系统的边缘缩放区(光标归系统、按下先交给 AppKit)。
- (BOOL)eventInSystemResizeZone:(NSEvent*)event;
- (void)attachOwner:(MacWindow*)owner;
- (void)detachOwner;
- (void)setMomoCursor:(NSCursor*)cursor hidden:(BOOL)hidden;
// view.setCapture(): 接管进行中的鼠标按下序列(按下发生在别的窗口)。
- (void)beginPointerCapture;
- (void)endPointerCapture;
- (void)refreshCursor;
@end


@implementation MomoContentView {
  MacWindow* _owner;             // 非拥有;MacWindow 析构时清空
  NSTrackingArea* _trackingArea;
  id _keyUpMonitor;
  id _captureMonitor;            // 显式捕获期间的本地事件监视器(见 beginPointerCapture)
  NSEvent* __weak _lastKeyUp;    // 最近处理过的 keyUp(去重: 监视器补发后 AppKit 若也投递, 只处理一次)
  uint32_t _buttons;             // 当前按下的按钮位图(逻辑: Control+左键记为次键)
  BOOL _controlClick;            // 当前左键按下是 Control+单击(macOS 惯例: 视为右键)
  BOOL _pointerAdded;            // 已发出 kAdd(尚未 kRemove)
  BOOL _inside;                  // 指针位于视图内
  NSCursor* _cursor;
  BOOL _wantCursorHidden;
  BOOL _cursorHideApplied;
  uint64_t _currentNativeEventId;  // 正在归一化的原生事件(0 = 合成事件)
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
  [self endPointerCapture];
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
    if (self.inLiveResize) {
      // 拖拽缩放: 等一帧新尺寸的内容, 与新窗口尺寸在同一事务里提交(否则旧帧被拉伸到松手)。
      _owner->SynchronizeLiveResize();
    }
  }
}

- (void)viewWillStartLiveResize {
  [super viewWillStartLiveResize];
  if (_owner != nullptr && _owner->metal_surface() != nullptr) {
    _owner->metal_surface()->SetLiveResizing(true);
  }
}

- (void)viewDidEndLiveResize {
  [super viewDidEndLiveResize];
  if (_owner != nullptr && _owner->metal_surface() != nullptr) {
    _owner->metal_surface()->SetLiveResizing(false);
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
  // 视图坐标(点, 左下原点)。
  const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  return [self pointerAtViewPoint:p screen:ScreenLocation(event) timestamp:TimestampUs(event) phase:phase];
}

// 与 pointerFromEvent 相同, 并带上正在处理的原生事件 id(框架据此把原始事件交给平台视图)。
// 只用于直接对应该原生事件的那个指针事件, 合成的 add/remove/补发抬起不带 id。
- (PointerData)nativePointerFromEvent:(NSEvent*)event phase:(PointerPhase)phase {
  PointerData data = [self pointerFromEvent:event phase:phase];
  data.native_event_id = _currentNativeEventId;
  return data;
}

// 交给 momo 的原生事件由平台视图的输入路由暂存并分配 id(框架回送时据此投递原始事件)。
- (uint64_t)recordNativeEvent:(NSEvent*)event {
  return _owner != nullptr ? [_owner->input_router() recordEvent:event] : 0;
}

- (void)platformViewPointer:(int64_t)viewId
              nativeEventId:(uint64_t)eventId
                     action:(PlatformViewPointerAction)action {
  if (_owner == nullptr) {
    return;
  }
  [_owner->input_router() dispatchToPlatformView:viewId nativeEventId:eventId action:action];
  if (action != PlatformViewPointerAction::kEvent) {
    [self refreshCursor];  // 指针归属变化: 光标归原生视图或回到 momo
  }
}

// screen: 主屏左上原点、y 向下的逻辑点(Quartz 全局坐标)→ 本视图坐标(点, 左下原点)。
- (NSPoint)viewPointFromScreen:(NSPoint)screen {
  return [self convertPoint:[self.window convertPointFromScreen:FlipScreenY(screen)] fromView:nil];
}

- (PointerData)pointerAtViewPoint:(NSPoint)p
                           screen:(NSPoint)screen
                        timestamp:(int64_t)timestamp
                            phase:(PointerPhase)phase {
  PointerData data;
  data.timestamp_us = timestamp;
  data.kind = PointerDeviceKind::kMouse;
  data.phase = phase;
  data.device = 0;
  // 视图坐标(点, 左下原点)→ 物理像素(左上原点)。
  const double scale = [self scale];
  data.physical_x = p.x * scale;
  data.physical_y = (self.bounds.size.height - p.y) * scale;
  data.screen_x = screen.x;
  data.screen_y = screen.y;
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
    [_owner->input_router() setPointerTracked:NO];
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
    [_owner->input_router() setPointerTracked:YES];
  }
  packet.push_back(data);
  _owner->delegate()->OnPointerPacket(std::move(packet));
}

// ---- 显式捕获(view.setCapture())----
// 按下发生在别的窗口(可能随后关闭), 本窗口接管这次按下序列: 在当前指针位置补发一次按下(框架据此
// 在本窗口开始新的手势序列, 引擎已把捕获转交给本视图), 之后的拖动与抬起经本地事件监视器全部交给本视图,
// 不论 AppKit 把它们投递给哪个窗口(与 Chromium 的 CocoaMouseCapture 相同)。抬起最后一个按键后释放。
- (void)beginPointerCapture {
  if (_owner == nullptr || self.window == nil) {
    return;
  }
  [self endPointerCapture];
  // pressedMouseButtons: bit0 左键, bit1 右键, bit2+ 其他 —— 与指针包的按钮位图一致。
  const uint32_t pressed = static_cast<uint32_t>([NSEvent pressedMouseButtons] & 0x7);
  if (pressed == 0) {
    return;  // 按键已松开: 没有可接管的序列
  }
  _buttons = pressed;
  _controlClick = NO;
  const NSPoint screen = FlipScreenY(NSEvent.mouseLocation);
  const int64_t now = static_cast<int64_t>(NSProcessInfo.processInfo.systemUptime * 1e6);
  [self dispatchPointer:[self pointerAtViewPoint:[self viewPointFromScreen:screen]
                                          screen:screen
                                       timestamp:now
                                           phase:PointerPhase::kDown]];
  MomoContentView* __weak weakSelf = self;
  // 捕获期间按键必然按住: 只有拖动与抬起。
  const NSEventMask mask = NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
                           NSEventMaskOtherMouseDragged | NSEventMaskLeftMouseUp |
                           NSEventMaskRightMouseUp | NSEventMaskOtherMouseUp;
  _captureMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                          handler:^NSEvent*(NSEvent* event) {
                                                            MomoContentView* view = weakSelf;
                                                            return view != nil ? [view handleCapturedEvent:event]
                                                                               : event;
                                                          }];
  MOMO_LOG(VERBOSE) << "Pointer capture taken (buttons " << pressed << ").";
}

- (void)endPointerCapture {
  if (_captureMonitor != nil) {
    [NSEvent removeMonitor:_captureMonitor];
    _captureMonitor = nil;
  }
}

// 捕获期间的鼠标事件: 位置按事件的屏幕坐标换算到本视图(事件可能属于别的窗口)。
// 发往本窗口的事件由这里独占处理(返回 nil, 不再经派发器重复投递);发往其他窗口的照常放行——
// 那些窗口借此复位自己的按下状态, 它们上报的事件在引擎里因捕获已转交而被丢弃。
- (NSEvent*)handleCapturedEvent:(NSEvent*)event {
  if (_owner == nullptr) {
    [self endPointerCapture];
    return event;
  }
  const NSPoint screen = ScreenLocation(event);
  const NSPoint p = [self viewPointFromScreen:screen];
  const int64_t ts = TimestampUs(event);
  switch (MomoClassifyMouseEvent(event)) {
    case MomoMouseEventKindUp: {
      _buttons &= ~ButtonBit(event);
      const BOOL last = _buttons == 0;
      [self dispatchPointer:[self pointerAtViewPoint:p screen:screen timestamp:ts
                                               phase:last ? PointerPhase::kUp : PointerPhase::kMove]];
      if (last) {
        [self endPointerCapture];
        if (!NSPointInRect(p, self.bounds)) {
          _inside = NO;
          [self dispatchPointer:[self pointerAtViewPoint:p screen:screen timestamp:ts
                                                   phase:PointerPhase::kRemove]];
        }
        MOMO_LOG(VERBOSE) << "Pointer capture released.";
      }
      break;
    }
    case MomoMouseEventKindDrag:
      [self dispatchPointer:[self pointerAtViewPoint:p screen:screen timestamp:ts
                                               phase:PointerPhase::kMove]];
      break;
    default:
      return event;
  }
  return event.window == self.window ? nil : event;
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
  // 键盘焦点: 按下落在 momo 内容上时由框架回送(platformViewPointer: 平台视图 0), 这里取回;
  // 落在平台视图上时由原生视图自己成为第一响应者。这里不能先取回: 点击已聚焦的网页会让它失焦再聚焦。
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
  [self dispatchPointer:[self nativePointerFromEvent:event
                                               phase:first ? PointerPhase::kDown : PointerPhase::kMove]];
}

- (void)handleButtonUp:(NSEvent*)event {
  _buttons &= ~[self logicalButtonBit:event];
  if (event.type == NSEventTypeLeftMouseUp) {
    _controlClick = NO;
  }
  const BOOL last = _buttons == 0;
  [self dispatchPointer:[self nativePointerFromEvent:event
                                               phase:last ? PointerPhase::kUp : PointerPhase::kMove]];
  if (last && ![self mouseIsInside]) {
    // 拖出视图后松开: 补发离开(带该抬起事件的 id: 平台视图在松开的位置离开)。
    _inside = NO;
    [self dispatchPointer:[self nativePointerFromEvent:event phase:PointerPhase::kRemove]];
  }
}

- (void)handleMove:(NSEvent*)event {
  _inside = YES;
  if (_buttons != 0 && event.type == NSEventTypeMouseMoved && [NSEvent pressedMouseButtons] == 0) {
    // 按下序列的抬起被嵌套事件循环(原生右键菜单等)吞掉: 补发抬起, 复位框架侧的指针状态。
    _buttons = 0;
    _controlClick = NO;
    [self dispatchPointer:[self pointerFromEvent:event phase:PointerPhase::kUp]];
  }
  [self dispatchPointer:[self nativePointerFromEvent:event
                                               phase:_buttons == 0 ? PointerPhase::kHover
                                                                   : PointerPhase::kMove]];
}

// 触控板滚动手势阶段 → SignalPhase(框架据此在整个手势内锁定接收者)。惯性阶段属于同一手势。
static SignalPhase ScrollPhaseOf(NSEvent* event) {
  const NSEventPhase phase = event.phase;
  const NSEventPhase momentum = event.momentumPhase;
  if (phase == NSEventPhaseNone && momentum == NSEventPhaseNone) return SignalPhase::kNone;
  if ((phase & (NSEventPhaseBegan | NSEventPhaseMayBegin)) != 0) return SignalPhase::kStart;
  if ((momentum & (NSEventPhaseEnded | NSEventPhaseCancelled)) != 0 ||
      (phase & NSEventPhaseCancelled) != 0) {
    return SignalPhase::kEnd;
  }
  return SignalPhase::kUpdate;
}

// 捏合/旋转等手势的阶段(没有惯性阶段)。
static SignalPhase GesturePhaseOf(NSEvent* event) {
  const NSEventPhase phase = event.phase;
  if (phase == NSEventPhaseNone) return SignalPhase::kNone;
  if ((phase & (NSEventPhaseBegan | NSEventPhaseMayBegin)) != 0) return SignalPhase::kStart;
  if ((phase & (NSEventPhaseEnded | NSEventPhaseCancelled)) != 0) return SignalPhase::kEnd;
  return SignalPhase::kUpdate;
}

// 其他带位置的输入(MomoMouseEventKindGesture)→ kGesture 指针事件。momo 只据此命中测试与锁定,
// 原始事件由框架回送后投递给原生视图(与鼠标事件同一条路线)。
- (void)handleGesture:(NSEvent*)event {
  PointerData data = [self nativePointerFromEvent:event phase:PointerPhase::kGesture];
  data.kind = PointerDeviceKind::kTrackpad;
  switch (event.type) {
    case NSEventTypeMagnify:
      data.gesture_kind = PointerGestureKind::kMagnify;
      data.gesture_value = event.magnification;
      data.signal_phase = GesturePhaseOf(event);
      break;
    case NSEventTypeRotate:
      data.gesture_kind = PointerGestureKind::kRotate;
      data.gesture_value = event.rotation;
      data.signal_phase = GesturePhaseOf(event);
      break;
    case NSEventTypeSwipe:
      data.gesture_kind = PointerGestureKind::kSwipe;
      data.scroll_delta_x = -event.deltaX;  // 与滚动一致: 视口方向
      data.scroll_delta_y = -event.deltaY;
      break;
    case NSEventTypeSmartMagnify:
      data.gesture_kind = PointerGestureKind::kSmartMagnify;
      break;
    case NSEventTypeQuickLook:
      data.gesture_kind = PointerGestureKind::kQuickLook;
      break;
    case NSEventTypePressure:
      // 按下序列内的压力变化: 框架沿按下时的命中路径投递(与拖动相同)。
      data.kind = PointerDeviceKind::kMouse;
      data.gesture_kind = PointerGestureKind::kPressure;
      data.gesture_value = event.pressure;
      data.gesture_stage = static_cast<int32_t>(event.stage);
      break;
    case NSEventTypeTabletPoint:
      data.kind = PointerDeviceKind::kStylus;
      data.gesture_kind = PointerGestureKind::kTabletPoint;
      data.gesture_value = event.pressure;
      break;
    case NSEventTypeBeginGesture:
      data.gesture_kind = PointerGestureKind::kBegin;
      data.signal_phase = SignalPhase::kStart;
      break;
    case NSEventTypeEndGesture:
      data.gesture_kind = PointerGestureKind::kEnd;
      data.signal_phase = SignalPhase::kEnd;
      break;
    default:
      return;
  }
  [self dispatchPointer:data];
}

- (void)handleScroll:(NSEvent*)event {
  PointerData data = [self nativePointerFromEvent:event phase:PointerPhase::kScroll];
  data.signal_phase = ScrollPhaseOf(event);
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
  // 零位移事件(手势开始/结束)也交给框架: 原生视图需要完整的手势阶段, 框架据此锁定接收者。
  [self dispatchPointer:data];
}

// ---- 鼠标入口(由 MomoWindow -sendEvent: 调用)----
// 全部鼠标事件都交给 momo;落在平台视图上的由框架命中测试后经 platformViewPointer: 回送, 再投递给原生视图。
// AppKit 经响应者方法投递来的鼠标事件一律忽略(见下方空实现), 保证 momo 只从这里收到事件。
- (BOOL)hasPressedButtons {
  return _buttons != 0;
}

- (void)handleMouseEvent:(NSEvent*)event {
  if (_owner == nullptr) {
    return;
  }
  const MomoMouseEventKind kind = MomoClassifyMouseEvent(event);
  if (kind == MomoMouseEventKindExit) {
    // 离开窗口: 指针 remove 带上该事件的 id, 框架据此让平台视图在窗口外的位置离开。
    if (_buttons == 0) {
      _currentNativeEventId = [self recordNativeEvent:event];
      [self momoMouseLeft:event];
      _currentNativeEventId = 0;
    }
    return;
  }
  if (_buttons == 0 && kind != MomoMouseEventKindDown) {
    // 没有按下序列时, 视图外的移动/滚动(如按下发生在标题栏后拖到这里之外)不属于 momo。
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    if (!NSPointInRect(p, self.bounds)) {
      return;
    }
  }
  const BOOL replayable = kind != MomoMouseEventKindEnter && kind != MomoMouseEventKindNone;
  _currentNativeEventId = replayable ? [self recordNativeEvent:event] : 0;
  switch (kind) {
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
    case MomoMouseEventKindGesture:
      [self handleGesture:event];
      break;
    case MomoMouseEventKindExit:
    case MomoMouseEventKindNone:
      break;
  }
  _currentNativeEventId = 0;
}

// 指针离开窗口: 对 momo 而言指针离开(hover 复位);回来时 dispatchPointer 自动补 kAdd。
- (void)momoMouseLeft:(NSEvent*)event {
  _inside = NO;
  if (_buttons == 0) {
    [self dispatchPointer:[self nativePointerFromEvent:event phase:PointerPhase::kRemove]];
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

// 系统的边缘缩放区(本视图坐标): 可缩放、非全屏的窗口内侧, 各边宽度由应用配置(view.setResizeOptions 的
// border, 缺省 5 点)。这里的光标归系统(显示为缩放形状), 左键按下也先交给 AppKit(MomoWindow -sendEvent:)——
// 光标与点击按同一个判断, 用户看到缩放光标就能缩放。
- (BOOL)inSystemResizeZone:(NSPoint)p {
  NSWindow* window = self.window;
  if (_owner == nullptr || window == nil || (window.styleMask & NSWindowStyleMaskResizable) == 0 ||
      (window.styleMask & NSWindowStyleMaskFullScreen) != 0) {
    return NO;
  }
  const NSRect b = self.bounds;
  if (!NSPointInRect(p, b)) {
    return NO;
  }
  const momo::engine::platform::WindowResizeOptions& o = _owner->resize_options();
  // 本视图不翻转: y 从底边向上。
  return p.x - NSMinX(b) < o.border_left || NSMaxX(b) - p.x < o.border_right ||
         p.y - NSMinY(b) < o.border_bottom || NSMaxY(b) - p.y < o.border_top;
}

- (BOOL)eventInSystemResizeZone:(NSEvent*)event {
  return [self inSystemResizeZone:[self convertPoint:event.locationInWindow fromView:nil]];
}

- (void)refreshCursor {
  BOOL inside = [self mouseIsInside];
  if (inside && _owner != nullptr && _owner->input_router().pointerOwned) {
    // 指针归平台视图(框架的进入/离开): 光标归原生视图管理(如 WKWebView 的 I-beam / 手形)。
    inside = NO;
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
    const NSPoint p = [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil];
    if ([self inSystemResizeZone:p]) {
      return;  // 系统的边缘缩放区: 光标归系统
    }
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

// MomoWindow: 鼠标与其他位置型输入的唯一入口(docs/05 §9 "输入")。全部交给 momo 内容视图(指针包附带
// 原生事件 id), 不经 AppKit 投递, 由框架命中测试决定接收者;落到平台视图上的事件由框架回送, 内容视图按 id
// 取回原始 NSEvent 交给平台视图宿主(容器), 再经 momoDeliverEvent:routedTo: 交给 AppKit 投递——投递期间
// 命中测试只落在该原生视图内, AppKit 完成按下捕获、第一响应者、手势识别器等。移动按跟踪区规则投递给 owner。
// 跟踪区事件按所属分流: 原生视图的不投递(由有序输入流推导), 内容视图的交给内容视图, 窗口装饰的交给 AppKit。
// 点击激活由窗口自己完成(按下不经 AppKit)。
// MomoWindow(NSWindow)与 MomoPanel(NSPanel)共用同一份实现(mac_window_body.inc)。

@interface MomoWindow : NSWindow <MomoWindowLike, MomoMouseRouting>
@end

// 面板(WindowSpec::panel, 同 Electron 的 type: 'panel'): NSPanel + NSWindowStyleMaskNonactivatingPanel。
// 可成为 key window 接收键盘, 但不成为 main window, 点击/聚焦它也不激活应用——其他窗口保持原状
// (应用在后台时不被带到前台, 主窗口的 main 外观不变)。
@interface MomoPanel : NSPanel <MomoWindowLike, MomoMouseRouting>
@end

@implementation MomoWindow {
  BOOL _systemTracking;        // 当前按下序列交给了 AppKit(红绿灯等窗口控件), 直到抬起
  int64_t _routedPlatformView;  // 正在经 AppKit 投递给的平台视图(MomoMouseRouting;0 = 无)
}
#include "embedder/macos/mac_window_body.inc"
@end

@implementation MomoPanel {
  BOOL _systemTracking;
  int64_t _routedPlatformView;
}
#include "embedder/macos/mac_window_body.inc"
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
  _owner->NotifyFrameChanged();
}
- (void)windowDidMove:(NSNotification*)notification {
  _owner->NotifyFrameChanged();
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
  resize_options_.resizable = spec.resizable;
  if (spec.resizable) {
    style |= NSWindowStyleMaskResizable;
  }
  const NSRect frame = NSMakeRect(0, 0, spec.size.width, spec.size.height);
  NSWindow<MomoWindowLike>* window = nil;
  if (spec.panel) {
    // 面板: 不激活应用即可成为 key(NonactivatingPanel 只对 NSPanel 有效);应用失活时不隐藏;
    // 层级由 SetAlwaysOnTop 决定(不默认浮动)。
    MomoPanel* panel = [[MomoPanel alloc] initWithContentRect:frame
                                                    styleMask:style | NSWindowStyleMaskNonactivatingPanel
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    panel.hidesOnDeactivate = NO;
    panel.floatingPanel = NO;
    panel.becomesKeyOnlyIfNeeded = NO;
    window = panel;
  } else {
    window = [[MomoWindow alloc] initWithContentRect:frame
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
  }
  window.canBecomeMain = !spec.panel;
  panel_ = spec.panel;
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
  window.momoContentView = view_;
  input_router_ = [[MomoPlatformViewInputRouter alloc] initWithPlatformViews:platform_views_
                                                                 contentView:view_];
  window.inputRouter = input_router_;
  platform_views_.inputRouter = input_router_;

  text_input_ = std::make_unique<MacTextInput>(view_, delegate_);
  [view_ attachOwner:this];
  [window_ makeFirstResponder:view_];

  observer_ = [[MomoWindowObserver alloc] initWithOwner:this];
  window_.delegate = observer_;
  if (spec.position) {
    SetPosition(*spec.position);
  } else {
    [window_ center];
  }
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
  input_router_ = nil;
  text_input_.reset();
  observer_ = nil;
  view_ = nil;
  window_ = nil;
}

void MacWindow::Show() {
  if (panel_) {
    // 面板: 带到最前并成为 key, 但不激活应用(其他窗口保持原状)。
    [window_ orderFrontRegardless];
    [window_ makeKeyWindow];
    return;
  }
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

void MacWindow::SetResizeOptions(const platform::WindowResizeOptions& options) {
  resize_options_ = options;
  NSWindowStyleMask style = window_.styleMask;
  style = options.resizable ? (style | NSWindowStyleMaskResizable) : (style & ~NSWindowStyleMaskResizable);
  if (style != window_.styleMask) {
    window_.styleMask = style;
  }
  [view_ refreshCursor];
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

// 屏幕框与定位都以内容区(momo 视图)为准: 屏幕坐标 − screen_frame 即窗口内坐标, 与窗口样式无关。
Rect MacWindow::screen_frame() const {
  const NSRect f = [window_ contentRectForFrameRect:window_.frame];
  return {f.origin.x, PrimaryScreenTop() - NSMaxY(f), f.size.width, f.size.height};
}

void MacWindow::SetPosition(Point top_left) {
  // 内容区左上角 → 窗口外框左上角(内容区之上可能还有标题栏)。
  const NSRect frame = window_.frame;
  const NSRect content = [window_ contentRectForFrameRect:frame];
  const CGFloat titlebar = NSMaxY(frame) - NSMaxY(content);
  [window_ setFrameTopLeftPoint:NSMakePoint(top_left.x - (content.origin.x - frame.origin.x),
                                            PrimaryScreenTop() - top_left.y + titlebar)];
}

void MacWindow::NotifyFrameChanged() {
  const Rect frame = screen_frame();
  if (frame == last_frame_) {
    return;
  }
  last_frame_ = frame;
  delegate_->OnWindowFrameChanged(frame);
}

void MacWindow::SetDragRegions(const std::vector<Rect>& rects) {
  std::vector<NSRect> regions;
  regions.reserve(rects.size());
  for (const Rect& r : rects) {
    regions.push_back(NSMakeRect(r.x, r.y, r.width, r.height));
  }
  ((NSWindow<MomoWindowLike>*)window_).dragRegions = std::move(regions);
}

void MacWindow::Activate() {
  if (window_.miniaturized) {
    [window_ deminiaturize:nil];
  }
  Show();
}

void MacWindow::DispatchPlatformViewPointer(int64_t view_id, uint64_t native_event_id,
                                            PlatformViewPointerAction action) {
  [view_ platformViewPointer:view_id nativeEventId:native_event_id action:action];
}

void MacWindow::CapturePointer() {
  [view_ beginPointerCapture];
}

void MacWindow::AddPlatformView(int64_t view_id, void* native_view) {
  id object = (__bridge id)native_view;
  if (![object isKindOfClass:[NSView class]]) {
    MOMO_LOG(ERROR) << "Platform view " << view_id << " is not an NSView; ignored.";
    return;
  }
  [platform_views_ addView:(NSView*)object viewId:view_id];
}

void MacWindow::RemovePlatformView(int64_t view_id) {
  [platform_views_ removeViewId:view_id];
  [input_router_ forgetPlatformView:view_id];
}

void MacWindow::FocusPlatformView(int64_t view_id) { [platform_views_ focusViewId:view_id]; }

void MacWindow::SynchronizeLiveResize() {
  // 嵌套(等待期间执行的平台任务又改变了窗口尺寸)时不再等待, 由外层的等待覆盖;应用可关闭等待(0)。
  const auto wait = std::chrono::duration_cast<TimeDelta>(
      std::chrono::duration<double, std::milli>(resize_options_.live_resize_frame_wait_ms));
  if (metal_surface_ == nullptr || resize_waiting_ || wait <= TimeDelta::zero() ||
      !metal_surface_->has_presented()) {
    NotifyMetricsChanged();
    return;
  }
  resize_waiting_ = true;
  // 先把新尺寸交给引擎(windowDidResize 随后的通知会被去重), 再等它画出这个尺寸的帧。
  NotifyMetricsChanged();
  EventLoop* loop = EventLoop::Current();
  const bool got_frame = metal_surface_->WaitForFrame(
      physical_size(), wait, [loop] {
        if (loop != nullptr) {
          loop->RunDueTasksNested();
        }
      });
  if (!got_frame) {
    MOMO_LOG(VERBOSE) << "Live resize: no frame of the new size within the timeout; content is stretched.";
  }
  resize_waiting_ = false;
}

void MacWindow::NotifyMetricsChanged() {
  const platform::ViewportMetrics current = metrics();
  if (current == last_metrics_) {
    return;
  }
  last_metrics_ = current;
  delegate_->OnMetricsChanged(current);
}

}  // namespace momo::embedder::macos
