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

// 触控板滚动手势阶段 → SignalPhase(框架据此在整个手势内锁定接收者)。惯性阶段属于同一手势。
SignalPhase ScrollPhaseOf(NSEvent* event) {
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
SignalPhase GesturePhaseOf(NSEvent* event) {
  const NSEventPhase phase = event.phase;
  if (phase == NSEventPhaseNone) return SignalPhase::kNone;
  if ((phase & (NSEventPhaseBegan | NSEventPhaseMayBegin)) != 0) return SignalPhase::kStart;
  if ((phase & (NSEventPhaseEnded | NSEventPhaseCancelled)) != 0) return SignalPhase::kEnd;
  return SignalPhase::kUpdate;
}

}  // namespace

// ---------------------------------------------------------------------------
// MomoContentView: 输入归一化 + NSTextInputClient + 光标。

// MomoWindow(NSWindow)与 MomoPanel(NSPanel)共有的接口, 实现见下方。
@protocol MomoWindowLike <NSObject>
@property(nonatomic, weak) NSView* momoContentView;
// 能否成为 main window。面板为 NO;始终可以成为 key window。
@property(nonatomic) BOOL canBecomeMain;
// 平台视图的输入路由: 派发器选中的原生视图的事件投递, 原生视图跟踪区事件的认领。
@property(nonatomic, weak) MomoPlatformViewInputRouter* inputRouter;
// 双击标题栏(拖动区域)的系统动作: 遵循"连按窗口标题栏以…"设置(缩放 / 最小化 / 无操作)。
- (void)titleBarDoubleClick;
@end

@interface MomoContentView : NSView <NSTextInputClient>
// 窗口内全部鼠标事件的入口(MomoWindow -sendEvent:): 翻译后交给 PointerDispatcher。
- (void)handleMouseEvent:(NSEvent*)event;
// 事件位于系统的边缘缩放区(光标归系统、按下先交给 AppKit)。
- (BOOL)eventInSystemResizeZone:(NSEvent*)event;
- (void)attachOwner:(MacWindow*)owner;
- (void)detachOwner;
@property(nonatomic, readonly) MacWindow* owner;  // 已脱离窗口时为 nullptr
- (void)setMomoCursor:(NSCursor*)cursor hidden:(BOOL)hidden;
- (void)refreshCursor;
// screen(主屏左上原点、y 向下的逻辑点)→ 本视图坐标(点, 左下原点)。
- (NSPoint)viewPointFromScreen:(NSPoint)screen;
@end

namespace {

momo::embedder::PointerDispatcher& Dispatcher() {
  static momo::embedder::PointerDispatcher* dispatcher = new momo::embedder::PointerDispatcher();
  return *dispatcher;
}

// AppKit 按钮位(1 << buttonNumber)→ 逻辑按钮位。Control+左键按下视为次键(与 AppKit 弹出上下文菜单的惯例
// 一致), 直到该左键抬起。
bool g_control_click = false;
uint32_t LogicalButton(NSEvent* event) {
  const uint32_t bit = ButtonBit(event);
  if (bit != 1u) return bit;
  if (event.type == NSEventTypeLeftMouseDown) {
    g_control_click = (event.modifierFlags & NSEventModifierFlagControl) != 0;
  }
  const bool control = g_control_click;
  if (event.type == NSEventTypeLeftMouseUp) g_control_click = false;
  return control ? (1u << 1) : bit;
}

// 其他带位置的输入(MomoMouseEventKindGesture)的手势字段。返回 false: 不处理的事件。
bool FillGesture(NSEvent* event, PointerData& data) {
  data.kind = PointerDeviceKind::kTrackpad;
  switch (event.type) {
    case NSEventTypeMagnify:
      data.gesture_kind = PointerGestureKind::kMagnify;
      data.gesture_value = event.magnification;
      data.signal_phase = GesturePhaseOf(event);
      return true;
    case NSEventTypeRotate:
      data.gesture_kind = PointerGestureKind::kRotate;
      data.gesture_value = event.rotation;
      data.signal_phase = GesturePhaseOf(event);
      return true;
    case NSEventTypeSwipe:
      data.gesture_kind = PointerGestureKind::kSwipe;
      data.scroll_delta_x = -event.deltaX;  // 与滚动一致: 视口方向
      data.scroll_delta_y = -event.deltaY;
      return true;
    case NSEventTypeSmartMagnify:
      data.gesture_kind = PointerGestureKind::kSmartMagnify;
      return true;
    case NSEventTypeQuickLook:
      data.gesture_kind = PointerGestureKind::kQuickLook;
      return true;
    case NSEventTypePressure:
      data.kind = PointerDeviceKind::kMouse;
      data.gesture_kind = PointerGestureKind::kPressure;
      data.gesture_value = event.pressure;
      data.gesture_stage = static_cast<int32_t>(event.stage);
      return true;
    case NSEventTypeTabletPoint:
      data.kind = PointerDeviceKind::kStylus;
      data.gesture_kind = PointerGestureKind::kTabletPoint;
      data.gesture_value = event.pressure;
      return true;
    case NSEventTypeBeginGesture:
      data.gesture_kind = PointerGestureKind::kBegin;
      data.signal_phase = SignalPhase::kStart;
      return true;
    case NSEventTypeEndGesture:
      data.gesture_kind = PointerGestureKind::kEnd;
      data.signal_phase = SignalPhase::kEnd;
      return true;
    default:
      return false;
  }
}

// NSEvent → PointerInput(只翻译, 不做决定)。返回 false: 不是指针输入。
bool TranslateMouseEvent(NSEvent* event, momo::embedder::PointerWindow* window,
                         momo::embedder::PointerInput& input) {
  using Type = momo::embedder::PointerInput::Type;
  input.window = window;
  input.native = (__bridge void*)event;
  PointerData& data = input.data;
  data.timestamp_us = TimestampUs(event);
  data.kind = PointerDeviceKind::kMouse;
  const NSPoint screen = ScreenLocation(event);
  data.screen_x = screen.x;
  data.screen_y = screen.y;
  const NSUInteger pressed = [NSEvent pressedMouseButtons];
  switch (MomoClassifyMouseEvent(event)) {
    case MomoMouseEventKindDown:
    case MomoMouseEventKindUp: {
      const bool down = MomoClassifyMouseEvent(event) == MomoMouseEventKindDown;
      input.type = down ? Type::kDown : Type::kUp;
      input.button = LogicalButton(event);
      const NSInteger n = event.buttonNumber;
      const NSUInteger own = n >= 0 && n < 64 ? (NSUInteger{1} << n) : 0;
      input.others_held = (pressed & ~own) != 0;
      return true;
    }
    case MomoMouseEventKindDrag:
      input.type = Type::kMove;
      input.others_held = true;  // 拖动本身说明按键按住(松开的抬起可能还在队列里)
      return true;
    case MomoMouseEventKindMove:
    case MomoMouseEventKindEnter:
      input.type = Type::kMove;
      input.others_held = pressed != 0;
      return true;
    case MomoMouseEventKindExit:
      input.type = Type::kLeave;
      return true;
    case MomoMouseEventKindScroll: {
      input.type = Type::kScroll;
      data.signal_phase = ScrollPhaseOf(event);
      double dx = event.scrollingDeltaX;
      double dy = event.scrollingDeltaY;
      if (event.hasPreciseScrollingDeltas) {
        data.kind = PointerDeviceKind::kTrackpad;  // 触控板 / Magic Mouse: 点
      } else {
        dx *= kScrollLinePixels;  // 普通滚轮: 行 → 点
        dy *= kScrollLinePixels;
      }
      // AppKit 的 delta 是"内容移动方向";统一为"视口滚动方向"(正 y = 向下滚, 与 Web deltaY 一致)。
      data.scroll_delta_x = -dx;
      data.scroll_delta_y = -dy;
      data.momentum = event.momentumPhase != NSEventPhaseNone;
      return true;
    }
    case MomoMouseEventKindGesture: {
      if (!FillGesture(event, data)) return false;
      // 压力、按住时的手写板点属于按下序列(AppKit 发给收到按下的视图)。
      const bool in_sequence = event.type == NSEventTypePressure ||
                               (event.type == NSEventTypeTabletPoint && pressed != 0);
      input.type = in_sequence ? Type::kPressure : Type::kGesture;
      return true;
    }
    case MomoMouseEventKindNone:
      return false;
  }
  return false;
}

void DispatchMouseEvent(NSEvent* event, MacWindow* window) {
  momo::embedder::PointerInput input;
  if (TranslateMouseEvent(event, window, input)) {
    Dispatcher().Dispatch(input);
  }
}

// 序列期间不属于任何 momo 窗口的按键/拖动事件(按下所在的窗口已关闭、拖到了窗口外的别的窗口上):
// 进程级本地监视器先于窗口派发把它们交给派发器(属于 momo 窗口的照常, 由窗口的 -sendEvent: 交给派发器)。
id g_sequence_monitor = nil;
void EnsureSequenceMonitor() {
  if (g_sequence_monitor != nil) return;
  const NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown |
                           NSEventMaskOtherMouseDown | NSEventMaskLeftMouseDragged |
                           NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
                           NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp |
                           NSEventMaskOtherMouseUp;
  g_sequence_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                              handler:^NSEvent*(NSEvent* event) {
                                                                return MomoRoutePointerSequenceEvent(event) ? nil : event;
                                                              }];
}

}  // namespace

@implementation MomoContentView {
  MacWindow* _owner;             // 非拥有;MacWindow 析构时清空
  NSTrackingArea* _trackingArea;
  id _keyUpMonitor;
  NSEvent* __weak _lastKeyUp;    // 最近处理过的 keyUp(去重: 监视器补发后 AppKit 若也投递, 只处理一次)
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
  EnsureSequenceMonitor();
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

- (MacWindow*)owner {
  return _owner;
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
// 决定接收方的是 PointerDispatcher(embedder/common/pointer_dispatcher.h, 进程内一个): 这里只把 NSEvent 翻译
// 后交给它;它经 MacWindow(PointerWindow)把事件交给 momo 内容或平台视图(MomoPlatformViewInputRouter)。
// 命中依据每一帧与画面同一个 CATransaction 生效的几何(MacWindow::HitTest), 即用户此刻看到的画面,
// 不经 ui 线程往返。

- (NSPoint)viewPointFromScreen:(NSPoint)screen {
  return [self convertPoint:[self.window convertPointFromScreen:FlipScreenY(screen)] fromView:nil];
}

- (void)handleMouseEvent:(NSEvent*)event {
  if (_owner == nullptr) {
    return;
  }
  DispatchMouseEvent(event, _owner);
  switch (MomoClassifyMouseEvent(event)) {
    case MomoMouseEventKindMove:
    case MomoMouseEventKindDrag:
    case MomoMouseEventKindEnter:
    case MomoMouseEventKindExit:
      [self refreshCursor];
      break;
    default:
      break;
  }
}

- (BOOL)mouseIsInside {
  const NSPoint p = [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil];
  return NSPointInRect(p, self.bounds);
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

// 系统的边缘缩放区(本视图坐标): 可缩放、非全屏的窗口内侧, 各边宽度由应用配置(momo/window setResizeBorder 的
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
  const momo::embedder::WindowResizeOptions o = _owner->resize_options();
  // 本视图不翻转: y 从底边向上。
  return p.x - NSMinX(b) < o.border_left || NSMaxX(b) - p.x < o.border_right ||
         p.y - NSMinY(b) < o.border_bottom || NSMaxY(b) - p.y < o.border_top;
}

- (BOOL)eventInSystemResizeZone:(NSEvent*)event {
  return [self inSystemResizeZone:[self convertPoint:event.locationInWindow fromView:nil]];
}

- (void)refreshCursor {
  BOOL inside = [self mouseIsInside];
  if (inside && _owner != nullptr && Dispatcher().PointerOwner(_owner).value_or(0) != 0) {
    // 指针归平台视图: 光标归原生视图管理(如 WKWebView 的 I-beam / 手形)。
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

// 进程级本地监视器的判定(测试也直接调用): 序列进行中、事件不属于任何(仍附着的)momo 窗口时, 交给派发器
// 并返回 true(不再照常派发);否则返回 false, 由事件所属窗口的 -sendEvent: 照常交给派发器。
bool MomoRoutePointerSequenceEvent(NSEvent* event) {
  if (!Dispatcher().has_sequence() || MomoClassifyMouseEvent(event) == MomoMouseEventKindNone) {
    return false;
  }
  NSWindow* window = event.window;
  if ([window conformsToProtocol:@protocol(MomoWindowLike)]) {
    MomoContentView* content = (MomoContentView*)((NSWindow<MomoWindowLike>*)window).momoContentView;
    if (content.owner != nullptr) {
      return false;
    }
  }
  DispatchMouseEvent(event, nullptr);
  return true;
}

bool MomoPointerSequenceActive() { return Dispatcher().has_sequence(); }

// ---------------------------------------------------------------------------

// MomoWindow: 鼠标与其他位置型输入的入口(docs/05 §9 "输入")。翻译后交给 PointerDispatcher, 不经 AppKit 投递;
// 派发器决定交给平台视图的, 由输入路由经 momoDeliverEvent:toPlatformView: 交给 AppKit 投递——投递期间
// 命中测试只落在该原生视图内, AppKit 完成按下捕获、第一响应者、手势识别器等。移动按跟踪区规则投递给 owner。
// 跟踪区事件按所属分流: 原生视图的不投递(由有序输入流推导), 内容视图的交给内容视图, 窗口装饰的交给 AppKit。
// 点击激活由窗口自己完成(按下不经 AppKit)。
// MomoWindow(NSWindow)与 MomoPanel(NSPanel)共用同一份实现(mac_window_body.inc)。

@interface MomoWindow : NSWindow <MomoWindowLike, MomoMouseRouting>
@end

// 辅助窗口(WindowSpec::non_activating, 同 Electron 的 type: 'panel'): NSPanel + NSWindowStyleMaskNonactivatingPanel。
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
  _owner->NotifyControlsChanged();
}
- (void)windowDidMove:(NSNotification*)notification {
  _owner->NotifyFrameChanged();
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification {
  _owner->NotifyControlsChanged();  // 全屏时红绿灯不占用内容区
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
  _owner->LayoutTrafficLights();
  _owner->NotifyControlsChanged();
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
  if (spec.non_activating) {
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
  window.canBecomeMain = !spec.non_activating;
  non_activating_ = spec.non_activating;
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
  MomoContentView* __weak weak_view = view_;
  // 几何变化(新的一帧已呈现、视图移除): 在呈现事务之外(下一轮主循环)请派发器重新判定指针下的接收方。
  platform_views_.geometryChanged = ^{
    dispatch_async(dispatch_get_main_queue(), ^{
      if (MacWindow* owner = weak_view.owner) Dispatcher().GeometryChanged(owner);
    });
  };
  platform_views_.pointerOwner = ^int64_t {
    MacWindow* owner = weak_view.owner;
    return owner != nullptr ? Dispatcher().PointerOwner(owner).value_or(-1) : -1;
  };

  text_input_ = std::make_unique<MacTextInput>(view_, delegate_);
  [view_ attachOwner:this];
  [window_ makeFirstResponder:view_];

  observer_ = [[MomoWindowObserver alloc] initWithOwner:this];
  window_.delegate = observer_;
  if (spec.position) {
    SetPosition(*spec.position).IgnoreError();
  } else {
    [window_ center];
  }
  last_metrics_ = metrics();
}

MacWindow::~MacWindow() {
  Dispatcher().RemoveWindow(this);
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
  if (non_activating_) {
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

void MacWindow::SetCursor(SystemCursor cursor) {
  NSCursor* ns_cursor = nil;
  switch (cursor) {
    case SystemCursor::kArrow: ns_cursor = [NSCursor arrowCursor]; break;
    case SystemCursor::kIBeam: ns_cursor = [NSCursor IBeamCursor]; break;
    case SystemCursor::kPointingHand: ns_cursor = [NSCursor pointingHandCursor]; break;
    case SystemCursor::kCrosshair: ns_cursor = [NSCursor crosshairCursor]; break;
    case SystemCursor::kResizeLeftRight: ns_cursor = [NSCursor resizeLeftRightCursor]; break;
    case SystemCursor::kResizeUpDown: ns_cursor = [NSCursor resizeUpDownCursor]; break;
    case SystemCursor::kNotAllowed: ns_cursor = [NSCursor operationNotAllowedCursor]; break;
    case SystemCursor::kHidden: break;
  }
  [view_ setMomoCursor:ns_cursor hidden:cursor == SystemCursor::kHidden];
}

TextInputConnection& MacWindow::text_input() { return *text_input_; }

uint32_t MacWindow::capabilities() const {
  return kCapabilityAlwaysOnTop | kCapabilityPosition | kCapabilityResizeOptions |
         kCapabilityHiddenTitleBar | kCapabilityWindowDrag | kCapabilityMoveLoop |
         kCapabilityPointerCapture | kCapabilityPlatformViews | kCapabilityNonActivating;
}

void* MacWindow::native_handle() const { return (__bridge void*)view_; }

Status MacWindow::SetAlwaysOnTop(bool on) {
  window_.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel;
  // 置顶窗口跟随用户切换到的桌面空间, 并可浮在全屏应用之上。
  window_.collectionBehavior = on ? (NSWindowCollectionBehaviorCanJoinAllSpaces |
                                     NSWindowCollectionBehaviorFullScreenAuxiliary)
                                  : NSWindowCollectionBehaviorDefault;
  return Status::Ok();
}

Status MacWindow::SetResizeOptions(const WindowResizeOptions& options) {
  resize_options_ = options;
  NSWindowStyleMask style = window_.styleMask;
  style = options.resizable ? (style | NSWindowStyleMaskResizable) : (style & ~NSWindowStyleMaskResizable);
  if (style != window_.styleMask) {
    window_.styleMask = style;
  }
  [view_ refreshCursor];
  return Status::Ok();
}

Status MacWindow::SetTitleBar(const TitleBarOptions& options) {
  title_bar_ = options;
  const bool hidden = options.hidden;
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
  NotifyControlsChanged();
  return Status::Ok();
}

std::optional<Rect> MacWindow::window_controls_rect() const {
  if (!title_bar_.hidden || (window_.styleMask & NSWindowStyleMaskFullScreen) != 0) {
    return std::nullopt;  // 系统标题栏可见(按钮不在内容区里)或全屏(按钮隐藏)
  }
  NSRect bounds = NSZeroRect;
  for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
    NSButton* button = [window_ standardWindowButton:kind];
    if (button == nil || button.hidden || button.superview == nil) continue;
    const NSRect r = [view_ convertRect:button.bounds fromView:button];
    bounds = NSIsEmptyRect(bounds) ? r : NSUnionRect(bounds, r);
  }
  if (NSIsEmptyRect(bounds)) {
    return std::nullopt;
  }
  // 本视图不翻转(左下原点)→ 内容区左上原点。左右各留出与左边距相同的空白, 应用据此避让。
  const CGFloat margin = NSMinX(bounds);
  return Rect{0, NSHeight(view_.bounds) - NSMaxY(bounds), NSMaxX(bounds) + margin, NSHeight(bounds)};
}

void MacWindow::NotifyControlsChanged() {
  const std::optional<Rect> controls = window_controls_rect();
  if (controls == last_controls_) {
    return;
  }
  last_controls_ = controls;
  delegate_->OnWindowControlsChanged(controls);
}

void MacWindow::LayoutTrafficLights() {
  const double traffic_center_y = title_bar_.height / 2;
  if (!title_bar_.hidden || traffic_center_y <= 0 ||
      (window_.styleMask & NSWindowStyleMaskFullScreen) != 0) {
    return;
  }
  NSButton* close = [window_ standardWindowButton:NSWindowCloseButton];
  NSView* titlebar = close.superview;         // NSTitlebarView
  NSView* container = titlebar.superview;     // NSTitlebarContainerView
  if (close == nil || container == nil) {
    return;
  }
  // 把标题栏容器的高度设为应用标题栏的高度, 按钮在其中垂直居中。
  const CGFloat height = static_cast<CGFloat>(traffic_center_y * 2);
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

Status MacWindow::SetPosition(Point top_left) {
  // 内容区左上角 → 窗口外框左上角(内容区之上可能还有标题栏)。
  const NSRect frame = window_.frame;
  const NSRect content = [window_ contentRectForFrameRect:frame];
  const CGFloat titlebar = NSMaxY(frame) - NSMaxY(content);
  [window_ setFrameTopLeftPoint:NSMakePoint(top_left.x - (content.origin.x - frame.origin.x),
                                            PrimaryScreenTop() - top_left.y + titlebar)];
  return Status::Ok();
}

void MacWindow::NotifyFrameChanged() {
  const Rect frame = screen_frame();
  if (frame == last_frame_) {
    return;
  }
  last_frame_ = frame;
  delegate_->OnWindowFrameChanged(frame);
}

Status MacWindow::StartWindowDrag(int64_t sequence_id) {
  if (!Dispatcher().IsMomoSequence(this, sequence_id)) {
    return Status::Ok();  // 序列已结束(往返期间用户已放手)或已不归本窗口
  }
  NSEvent* down = sequence_down_;
  if (sequence_down_id_ != sequence_id || down == nil || down.type != NSEventTypeLeftMouseDown) {
    return InvalidArgumentError("startDrag: the sequence did not start with a left press in this window");
  }
  NSWindow<MomoWindowLike>* window = (NSWindow<MomoWindowLike>*)window_;
  if (down.clickCount == 2) {
    [window titleBarDoubleClick];
    return Status::Ok();
  }
  // 系统拖动(含贴边、跨屏等系统行为)。拖动循环吞掉抬起: 返回时按键已松开则在这里结束序列
  // (否则由派发器的过期判定在下一个事件上结束)。
  [window performWindowDragWithEvent:down];
  if (([NSEvent pressedMouseButtons] & 1) == 0) {
    const NSPoint screen = FlipScreenY(NSEvent.mouseLocation);
    Dispatcher().EndSequence(this, {screen.x, screen.y},
                             static_cast<int64_t>(NSProcessInfo.processInfo.systemUptime * 1e6));
  }
  return Status::Ok();
}

Status MacWindow::StartMoveLoop(Point grab_offset) {
  Dispatcher().StartMoveLoop(this, grab_offset);  // 不是进行中序列的接收方时什么也不做
  return Status::Ok();
}

void MacWindow::EndMoveLoop() { Dispatcher().EndMoveLoop(this); }

void MacWindow::Activate() {
  if (window_.miniaturized) {
    [window_ deminiaturize:nil];
  }
  Show();
}

StatusOr<bool> MacWindow::CapturePointer(int64_t sequence_id) {
  return Dispatcher().Capture(this, sequence_id);
}

void MacWindow::ReleasePointerCapture() { Dispatcher().ReleaseCapture(this); }

// ---- PointerWindow ----

std::optional<int64_t> MacWindow::HitTest(Point screen) const {
  if (!window_.visible) {
    return std::nullopt;
  }
  // 被本应用的别的窗口遮住的地方不属于本窗口(重叠的窗口之间只有最上层的那个接收指针)。别的应用的窗口
  // 盖住时系统根本不把事件交给本应用, 不必考虑。
  const NSPoint cocoa = FlipScreenY(NSMakePoint(screen.x, screen.y));
  for (NSNumber* number in [NSWindow windowNumbersWithOptions:0]) {  // 本应用的屏幕上窗口, 由前到后
    NSWindow* w = [NSApp windowWithWindowNumber:number.integerValue];
    if (w != nil && !w.ignoresMouseEvents && NSPointInRect(cocoa, w.frame)) {
      if (w != window_) return std::nullopt;
      break;
    }
  }
  const NSPoint in_window = [window_ convertPointFromScreen:cocoa];
  if (!NSPointInRect([view_ convertPoint:in_window fromView:nil], view_.bounds)) {
    return std::nullopt;
  }
  return [platform_views_ platformViewAtWindowPoint:in_window];
}

void MacWindow::SendToMomo(platform::PointerDataPacket packet, const PointerInput& input) {
  // 屏幕坐标 → 本视图的物理像素(左上原点);滚动位移 点 → 物理像素。
  const double scale = device_pixel_ratio() > 0 ? device_pixel_ratio() : 1.0;
  const CGFloat height = view_.bounds.size.height;
  for (platform::PointerData& data : packet) {
    const NSPoint p = [view_ viewPointFromScreen:NSMakePoint(data.screen_x, data.screen_y)];
    data.physical_x = p.x * scale;
    data.physical_y = (height - p.y) * scale;
    data.scroll_delta_x *= scale;
    data.scroll_delta_y *= scale;
    if (data.phase == PointerPhase::kDown) {
      // 序列的按下(startDrag 据此开始系统拖动)。按下由 momo 内容接收: 键盘焦点交还 momo(之前可能在网页里)。
      sequence_down_ = (__bridge NSEvent*)input.native;
      sequence_down_id_ = data.sequence_id;
      if ([platform_views_ viewIdForResponder:window_.firstResponder] != 0) {
        [window_ makeFirstResponder:view_];
      }
    }
  }
  delegate_->OnPointerPacket(std::move(packet));
}

void MacWindow::SendToPlatformView(int64_t view, const PointerInput& input, PointerPhase phase) {
  NSEvent* event = (__bridge NSEvent*)input.native;
  if (event != nil) {
    [input_router_ deliverEvent:event toPlatformView:view phase:phase windowWasKey:window_.isKeyWindow];
  }
}

void MacWindow::SetHoveredPlatformView(int64_t view, const PointerInput& input) {
  // 别的窗口的事件的窗口坐标不属于本窗口: 取当前指针位置。
  NSEvent* event = input.window == this ? (__bridge NSEvent*)input.native : nil;
  [input_router_ setHoveredPlatformView:view event:event];
  [view_ refreshCursor];
}

void MacWindow::MoveTo(Point top_left) { SetPosition(top_left).IgnoreError(); }

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
  // 嵌套(等待期间执行的平台任务又改变了窗口尺寸)时不再等待, 由外层的等待覆盖。
  // 等待上限: 应用在此之内出帧则缩放无拉伸, 否则暂时拉伸旧内容(窗口拖动保持跟手)。
  constexpr TimeDelta kLiveResizeFrameWait = std::chrono::milliseconds(100);
  const TimeDelta wait = kLiveResizeFrameWait;
  if (metal_surface_ == nullptr || resize_waiting_ || !metal_surface_->has_presented()) {
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
