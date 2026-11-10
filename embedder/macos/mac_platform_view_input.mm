#include "embedder/macos/mac_platform_view_input.h"

#include <map>

#include "base/logging.h"
#include "embedder/macos/mac_mouse_events.h"
#include "embedder/macos/mac_platform_views.h"

using momo::engine::platform::PlatformViewPointerAction;

namespace {

// 原生事件环形缓冲的容量: 框架通常在同一帧内回送;溢出时最旧的事件不再能投递给平台视图。
constexpr size_t kNativeEventSlots = 128;

struct NativeMouseEvent {
  uint64_t id = 0;
  NSEvent* event = nil;
  BOOL wasKey = NO;  // 事件到达时窗口是否已是 key(随后窗口会在原始事件上被激活)
};

// 一个平台视图的输入状态。
struct PlatformViewInput {
  // 本平台视图收到的按下序列(哪个平台视图接收由框架决定;原生视图内部的捕获由 AppKit 在投递时完成)。
  NSUInteger pressedButtons = 0;  // AppKit 按钮位: 1 << buttonNumber
  BOOL swallowing = NO;           // 本序列只用于激活窗口(视图不接受 first mouse), 不投递
  // 原生视图跟踪区的状态(进入/离开/光标更新)只由有序输入流推导(见 updateTracking)。
  BOOL owned = NO;                // 框架认为指针在本平台视图上
  NSHashTable<NSTrackingArea*>* inside = nil;  // 已投递进入、尚未投递离开的跟踪区
};

NSUInteger AppKitButtonBit(NSEvent* event) {
  const NSInteger n = event.buttonNumber;
  return n >= 0 && n < 32 ? (1u << n) : 0;
}

// 跟踪区当前是否生效(按其激活条件, 与 AppKit 相同)。
BOOL TrackingAreaIsActive(NSTrackingArea* area, NSView* view) {
  const NSTrackingAreaOptions o = area.options;
  NSWindow* window = view.window;
  if (o & NSTrackingActiveAlways) return YES;
  if ((o & NSTrackingActiveInActiveApp) && NSApp.isActive) return YES;
  if ((o & NSTrackingActiveInKeyWindow) && window.isKeyWindow) return YES;
  if ((o & NSTrackingActiveWhenFirstResponder) && window.firstResponder == view) return YES;
  return NO;
}

NSRect TrackingRect(NSTrackingArea* area, NSView* view) {
  return (area.options & NSTrackingInVisibleRect) ? view.visibleRect : area.rect;
}

// 以 like 的位置/时间/修饰键合成 area 的跟踪事件(进入/离开/光标更新)并投递给其 owner。
void SendTrackingEvent(NSEventType type, NSTrackingArea* area, NSEvent* like) {
  NSEvent* event = [NSEvent enterExitEventWithType:type
                                          location:like.locationInWindow
                                     modifierFlags:like.modifierFlags
                                         timestamp:like.timestamp
                                      windowNumber:like.windowNumber
                                           context:nil
                                       eventNumber:0
                                    trackingNumber:(NSInteger)(__bridge void*)area
                                          userData:(__bridge void*)area.userInfo];
  id owner = area.owner;
  switch (type) {
    case NSEventTypeMouseEntered:
      if ([owner respondsToSelector:@selector(mouseEntered:)]) [owner mouseEntered:event];
      break;
    case NSEventTypeMouseExited:
      if ([owner respondsToSelector:@selector(mouseExited:)]) [owner mouseExited:event];
      break;
    case NSEventTypeCursorUpdate:
      if ([owner respondsToSelector:@selector(cursorUpdate:)]) [owner cursorUpdate:event];
      break;
    default:
      break;
  }
}

}  // namespace

@implementation MomoPlatformViewInputRouter {
  MomoPlatformViews* __weak _views;
  NSView* __weak _content;
  NativeMouseEvent _events[kNativeEventSlots];
  uint64_t _lastEventId;
  std::map<int64_t, PlatformViewInput> _inputs;
}

- (instancetype)initWithPlatformViews:(MomoPlatformViews*)views contentView:(NSView*)content {
  if ((self = [super init])) {
    _views = views;
    _content = content;
  }
  return self;
}

// ---- 原生事件环形缓冲 ----

- (uint64_t)recordEvent:(NSEvent*)event {
  const uint64_t id = ++_lastEventId;
  NativeMouseEvent& slot = _events[id % kNativeEventSlots];
  slot.id = id;
  slot.event = event;
  NSView* content = _content;
  slot.wasKey = content.window.isKeyWindow;
  return id;
}

- (const NativeMouseEvent*)eventWithId:(uint64_t)id {
  const NativeMouseEvent& slot = _events[id % kNativeEventSlots];
  return id != 0 && slot.id == id && slot.event != nil ? &slot : nullptr;
}

- (PlatformViewInput&)inputFor:(int64_t)viewId {
  PlatformViewInput& input = _inputs[viewId];
  if (input.inside == nil) {
    input.inside = [NSHashTable hashTableWithOptions:NSPointerFunctionsStrongMemory];
  }
  return input;
}

- (void)forgetPlatformView:(int64_t)viewId {
  _inputs.erase(viewId);
}

// ---- 框架的处置 ----

- (void)dispatchToPlatformView:(int64_t)viewId
                 nativeEventId:(uint64_t)eventId
                        action:(PlatformViewPointerAction)action {
  const NativeMouseEvent* native = [self eventWithId:eventId];
  switch (action) {
    case PlatformViewPointerAction::kEvent: {
      if (native == nullptr) {
        MOMO_LOG(VERBOSE) << "Native mouse event " << eventId << " expired before delivery.";
        return;
      }
      if (viewId == 0) {
        // 按下由 momo 内容消费: 若键盘焦点在某个平台视图里, 取回(否则键盘事件仍发往原生视图)。
        NSView* content = _content;
        NSWindow* window = content.window;
        if (MomoClassifyMouseEvent(native->event) == MomoMouseEventKindDown && window != nil &&
            window.firstResponder != content) {
          [window makeFirstResponder:content];
        }
        return;
      }
      [self deliverEvent:native->event toPlatformView:viewId windowWasKey:native->wasKey];
      return;
    }
    case PlatformViewPointerAction::kEnter:
    case PlatformViewPointerAction::kExit:
      // 触发进入/离开的原生事件提供位置与时间(移出窗口时为窗口外的点);已过期或合成时取当前指针位置。
      [self setPointerOwned:action == PlatformViewPointerAction::kEnter
               platformView:viewId
                      event:native != nullptr ? native->event : nil];
      return;
  }
}

- (void)setPointerOwned:(BOOL)owned platformView:(int64_t)viewId event:(NSEvent*)event {
  PlatformViewInput& input = [self inputFor:viewId];
  if (input.owned == owned) {
    return;
  }
  input.owned = owned;
  [self updateTracking:viewId event:event dragging:input.pressedButtons != 0];
}

- (BOOL)pointerOwnedByPlatformView:(int64_t)viewId {
  auto it = _inputs.find(viewId);
  return it != _inputs.end() && it->second.owned;
}

- (BOOL)pointerOwned {
  MomoPlatformViews* views = _views;
  for (const auto& [id, input] : _inputs) {
    if (input.owned && [views hostedNativeForPlatformView:id] != nil) return YES;
  }
  return NO;
}

// ---- 投递 ----

// 经 AppKit 的正常投递交给原生视图: 窗口在投递期间把命中测试限定在该原生视图子树里。
- (void)replay:(NSEvent*)event toPlatformView:(int64_t)viewId {
  NSView* content = _content;
  id window = content.window;
  if ([window conformsToProtocol:@protocol(MomoMouseRouting)]) {
    [(id<MomoMouseRouting>)window momoDeliverEvent:event toPlatformView:viewId];
  }
}

- (void)deliverEvent:(NSEvent*)event toPlatformView:(int64_t)viewId windowWasKey:(BOOL)wasKey {
  MomoPlatformViews* views = _views;
  if ([views hostedNativeForPlatformView:viewId] == nil) {
    return;  // 隐藏、已移除或已被其他窗口接管
  }
  PlatformViewInput& input = [self inputFor:viewId];
  switch (MomoClassifyMouseEvent(event)) {
    case MomoMouseEventKindDown: {
      [self updateTracking:viewId event:event dragging:input.pressedButtons != 0];
      const NSUInteger bit = AppKitButtonBit(event);
      // 之前的按下没有收到抬起(被原生菜单等嵌套事件循环吞掉): 开始新的序列。
      if (input.pressedButtons != 0 && (input.pressedButtons & ~bit & [NSEvent pressedMouseButtons]) == 0) {
        input.pressedButtons = 0;
      }
      if (input.pressedButtons == 0) {
        // first mouse: 事件到达时窗口不是 key(窗口已在原始事件上被激活), 由被点到的视图决定这次点击
        // 是否只用于激活窗口——不接受时整个按下序列都不投递(同 AppKit)。
        NSView* hit = [views nativeHitTestForPlatformView:viewId windowPoint:event.locationInWindow];
        input.swallowing = hit == nil || (!wasKey && ![hit acceptsFirstMouse:event]);
      }
      input.pressedButtons |= bit;
      if (!input.swallowing) [self replay:event toPlatformView:viewId];
      return;
    }
    case MomoMouseEventKindDrag:
    case MomoMouseEventKindUp: {
      if (input.pressedButtons == 0) {
        return;  // 没有在本平台视图开始的按下序列
      }
      const BOOL up = MomoClassifyMouseEvent(event) == MomoMouseEventKindUp;
      if (!up) [self updateTracking:viewId event:event dragging:YES];
      if (!input.swallowing) [self replay:event toPlatformView:viewId];
      if (up) {
        input.pressedButtons &= ~AppKitButtonBit(event);
        if (input.pressedButtons == 0) input.swallowing = NO;
        // 抬起后补上拖动期间推迟的跟踪区更新。
        [self updateTracking:viewId event:event dragging:input.pressedButtons != 0];
      }
      return;
    }
    case MomoMouseEventKindMove:
      [self updateTracking:viewId event:event dragging:NO];  // 先进入/离开, 再移动(与 AppKit 相同)
      [self deliverMove:event toPlatformView:viewId];
      return;
    case MomoMouseEventKindScroll:
      [self updateTracking:viewId event:event dragging:input.pressedButtons != 0];
      [self replay:event toPlatformView:viewId];
      return;
    case MomoMouseEventKindGesture:
      if (event.type == NSEventTypePressure ||
          (event.type == NSEventTypeTabletPoint && input.pressedButtons != 0)) {
        // 按下序列内: AppKit 发给收到按下的视图。
        if (input.pressedButtons != 0 && !input.swallowing) [self replay:event toPlatformView:viewId];
      } else {
        [self replay:event toPlatformView:viewId];  // 触控板手势、智能缩放、查询、手写板点
      }
      return;
    case MomoMouseEventKindEnter:
    case MomoMouseEventKindExit:
    case MomoMouseEventKindNone:
      return;
  }
}

// 移动: AppKit 把移动投递给"要移动事件"(NSTrackingMouseMoved)的跟踪区 owner, 以及(窗口接受移动事件时)
// first responder——不是命中视图。WKWebView 本身不实现 mouseMoved:, 悬停靠它跟踪区的 owner
// (WebKit 内部的观察者对象)接收。这里照 AppKit 的跟踪区规则投递给原生视图子树中包含指针、当前生效的
// 跟踪区, 不论原生视图是否聚焦;没有这样的跟踪区时交给命中视图的 mouseMoved:(沿响应链冒泡, 到平台视图为止)。
- (void)deliverMove:(NSEvent*)event toPlatformView:(int64_t)viewId {
  MomoPlatformViews* views = _views;
  NSView* native = [views hostedNativeForPlatformView:viewId];
  if (native == nil) {
    return;
  }
  BOOL delivered = NO;
  NSMutableArray<NSView*>* pending = [NSMutableArray arrayWithObject:native];
  NSHashTable* owners = [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
  while (pending.count > 0) {
    NSView* view = pending.lastObject;
    [pending removeLastObject];
    if (view.hidden) continue;
    const NSPoint p = [view convertPoint:event.locationInWindow fromView:nil];
    for (NSTrackingArea* area in view.trackingAreas) {
      if ((area.options & NSTrackingMouseMoved) == 0 || !TrackingAreaIsActive(area, view)) continue;
      id owner = area.owner;
      if (!NSPointInRect(p, TrackingRect(area, view)) || [owners containsObject:owner] ||
          ![owner respondsToSelector:@selector(mouseMoved:)]) {
        continue;
      }
      [owners addObject:owner];  // 同一 owner 只收一次(多个跟踪区共用 owner)
      [owner mouseMoved:event];
      delivered = YES;
    }
    [pending addObjectsFromArray:view.subviews];
  }
  if (!delivered) {
    if (NSView* hit = [views nativeHitTestForPlatformView:viewId windowPoint:event.locationInWindow]) {
      [hit mouseMoved:event];
    }
  }
}

// ---- 跟踪区(进入/离开/光标更新)----

// 输入流中没有对应原生事件时(已过期、视图移出树)的位置: 当前指针位置。
- (NSEvent*)eventAtCurrentLocation {
  NSView* content = _content;
  NSWindow* window = content.window;
  return [NSEvent enterExitEventWithType:NSEventTypeMouseExited
                                location:window != nil ? window.mouseLocationOutsideOfEventStream : NSZeroPoint
                           modifierFlags:NSEvent.modifierFlags
                               timestamp:NSProcessInfo.processInfo.systemUptime
                            windowNumber:window.windowNumber
                                 context:nil
                             eventNumber:0
                          trackingNumber:0
                                userData:nullptr];
}

// 按输入流中的一个事件更新平台视图的跟踪区状态(与 AppKit 的规则相同): 指针归该平台视图、位于跟踪区内、
// 跟踪区生效 → 在内。按下拖动期间只更新带 NSTrackingEnabledDuringMouseDrag 的跟踪区, 其余的保持原状
// 直到抬起(AppKit 同样推迟)。先离开后进入;进入时带 NSTrackingCursorUpdate 的跟踪区补一次光标更新。
// 已从视图上移除的跟踪区不再投递(AppKit 也不会为移除的跟踪区发离开)。
// AppKit 自己为这些跟踪区生成的事件不投递(routeTrackingEvent:): 它们同步产生, 而框架转发的移动要经
// ui 线程往返, 两者没有先后关系——直接移出窗口时 AppKit 的离开先到、之前的移动后到, 原生视图会在
// "离开"之后又被移动带回悬停状态。同 Chromium: 渲染器看到的 MouseLeave 与移动在同一个有序输入队列里。
- (void)updateTracking:(int64_t)viewId event:(NSEvent*)event dragging:(BOOL)dragging {
  if (event == nil) event = [self eventAtCurrentLocation];
  PlatformViewInput& input = [self inputFor:viewId];
  MomoPlatformViews* views = _views;
  NSView* native = [views hostedNativeForPlatformView:viewId];
  NSHashTable<NSTrackingArea*>* installed =
      [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
  NSMutableArray<NSTrackingArea*>* entering = [NSMutableArray array];
  NSMutableArray<NSTrackingArea*>* leaving = [NSMutableArray array];
  if (native != nil) {
    NSMutableArray<NSView*>* pending = [NSMutableArray arrayWithObject:native];
    while (pending.count > 0) {
      NSView* view = pending.lastObject;
      [pending removeLastObject];
      const NSPoint p = [view convertPoint:event.locationInWindow fromView:nil];
      const BOOL visible = !view.hidden;
      for (NSTrackingArea* area in view.trackingAreas) {
        const NSTrackingAreaOptions o = area.options;
        if ((o & (NSTrackingMouseEnteredAndExited | NSTrackingCursorUpdate)) == 0) continue;
        [installed addObject:area];
        if (dragging && (o & NSTrackingEnabledDuringMouseDrag) == 0) continue;  // 抬起后再更新
        const BOOL in = input.owned && visible && TrackingAreaIsActive(area, view) &&
                        NSPointInRect(p, TrackingRect(area, view));
        const BOOL was = [input.inside containsObject:area];
        if (in && !was) [entering addObject:area];
        if (!in && was) [leaving addObject:area];
      }
      if (visible) [pending addObjectsFromArray:view.subviews];
    }
  }
  for (NSTrackingArea* area in input.inside.allObjects) {
    if (![installed containsObject:area]) [input.inside removeObject:area];
  }
  for (NSTrackingArea* area in leaving) {
    [input.inside removeObject:area];
    if (area.options & NSTrackingMouseEnteredAndExited) SendTrackingEvent(NSEventTypeMouseExited, area, event);
  }
  for (NSTrackingArea* area in entering) {
    [input.inside addObject:area];
    if (area.options & NSTrackingMouseEnteredAndExited) SendTrackingEvent(NSEventTypeMouseEntered, area, event);
    if (area.options & NSTrackingCursorUpdate) SendTrackingEvent(NSEventTypeCursorUpdate, area, event);
  }
}

- (BOOL)routeTrackingEvent:(NSEvent*)event {
  MomoPlatformViews* views = _views;
  return event.trackingArea != nil && [views platformViewOwningTrackingArea:event.trackingArea] != 0;
}

@end
