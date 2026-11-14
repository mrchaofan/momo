#include "embedder/macos/mac_platform_view_input.h"

#include <map>

#include "base/logging.h"
#include "embedder/macos/mac_mouse_events.h"
#include "embedder/macos/mac_platform_views.h"

using momo::engine::platform::PointerPhase;

@implementation MomoPlatformViewInputRouter {
  MomoPlatformViews* __weak _views;
  NSView* __weak _content;
  int64_t _hoverView;     // 指针所在的平台视图(派发器的悬停目标;0 = 无)
  int64_t _sequenceView;  // 按下序列的接收方(0 = 没有原生视图的序列)
  BOOL _swallowing;       // 本序列只用于激活窗口(视图不接受 first mouse), 不投递
  // 每个平台视图已投递进入、尚未投递离开的跟踪区(只由有序输入流推导, 见 updateTracking)。
  std::map<int64_t, NSHashTable<NSTrackingArea*>*> _inside;
}

- (instancetype)initWithPlatformViews:(MomoPlatformViews*)views contentView:(NSView*)content {
  if ((self = [super init])) {
    _views = views;
    _content = content;
  }
  return self;
}

- (NSHashTable<NSTrackingArea*>*)insideOf:(int64_t)viewId {
  __strong NSHashTable<NSTrackingArea*>*& inside = _inside[viewId];
  if (inside == nil) {
    inside = [NSHashTable hashTableWithOptions:NSPointerFunctionsStrongMemory];
  }
  return inside;
}

- (void)forgetPlatformView:(int64_t)viewId {
  _inside.erase(viewId);
  if (_hoverView == viewId) _hoverView = 0;
  if (_sequenceView == viewId) _sequenceView = 0;
}

- (void)setHoveredPlatformView:(int64_t)viewId event:(NSEvent*)event {
  if (viewId == _hoverView) {
    return;
  }
  const int64_t old = _hoverView;
  _hoverView = viewId;
  if (old != 0) [self updateTracking:old event:event dragging:old == _sequenceView];
  if (viewId != 0) [self updateTracking:viewId event:event dragging:viewId == _sequenceView];
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

- (void)deliverEvent:(NSEvent*)event
      toPlatformView:(int64_t)viewId
               phase:(PointerPhase)phase
        windowWasKey:(BOOL)wasKey {
  MomoPlatformViews* views = _views;
  if ([views hostedNativeForPlatformView:viewId] == nil) {
    return;  // 隐藏、已移除或已被其他窗口接管
  }
  switch (phase) {
    case PointerPhase::kDown: {
      [self updateTracking:viewId event:event dragging:NO];
      // first mouse: 事件到达时窗口不是 key(窗口随后在原始事件上被激活), 由被点到的视图决定这次点击
      // 是否只用于激活窗口——不接受时整个按下序列都不投递(同 AppKit)。
      NSView* hit = [views nativeHitTestForPlatformView:viewId windowPoint:event.locationInWindow];
      _sequenceView = viewId;
      _swallowing = hit == nil || (!wasKey && ![hit acceptsFirstMouse:event]);
      if (!_swallowing) [self replay:event toPlatformView:viewId];
      return;
    }
    case PointerPhase::kMove:  // 序列内: 拖动, 或另一个按键的按下/抬起
      if (MomoClassifyMouseEvent(event) == MomoMouseEventKindDrag) {
        [self updateTracking:viewId event:event dragging:YES];
      }
      if (!_swallowing) [self replay:event toPlatformView:viewId];
      return;
    case PointerPhase::kUp:
      if (!_swallowing) [self replay:event toPlatformView:viewId];
      _sequenceView = 0;
      _swallowing = NO;
      [self updateTracking:viewId event:event dragging:NO];  // 补上拖动期间推迟的跟踪区更新
      return;
    case PointerPhase::kHover:
      [self updateTracking:viewId event:event dragging:NO];  // 先进入/离开, 再移动(与 AppKit 相同)
      [self deliverMove:event toPlatformView:viewId];
      return;
    case PointerPhase::kScroll:
      [self updateTracking:viewId event:event dragging:viewId == _sequenceView];
      [self replay:event toPlatformView:viewId];
      return;
    case PointerPhase::kGesture:
      // 属于按下序列的(压力、按住时的手写板点)随序列一起被吞掉。
      if (!(_swallowing && viewId == _sequenceView)) [self replay:event toPlatformView:viewId];
      return;
    case PointerPhase::kAdd:
    case PointerPhase::kRemove:
    case PointerPhase::kCancel:
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
  if (native == nil || MomoDeliverMouseMoved(native, event, nil)) {
    return;
  }
  if (NSView* hit = [views nativeHitTestForPlatformView:viewId windowPoint:event.locationInWindow]) {
    [hit mouseMoved:event];
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

// 按输入流中的一个事件更新平台视图的跟踪区状态(MomoUpdateTrackingAreas, 规则与 AppKit 相同):
// 指针悬停在该平台视图上(派发器的悬停目标)才可能在内。
// AppKit 自己为这些跟踪区生成的事件不投递(routeTrackingEvent:): 它们按 AppKit 自己的几何产生, 与派发器
// 对画面(遮挡、z 序)的判定没有先后关系, 投递会让原生视图在派发器判定的"离开"之后又回到悬停状态。
// 同 Chromium: 渲染器看到的 MouseLeave 与移动在同一个有序输入队列里。
- (void)updateTracking:(int64_t)viewId event:(NSEvent*)event dragging:(BOOL)dragging {
  if (event == nil) event = [self eventAtCurrentLocation];
  MomoPlatformViews* views = _views;
  MomoUpdateTrackingAreas([views hostedNativeForPlatformView:viewId], nil, event, viewId == _hoverView, dragging,
                          [self insideOf:viewId]);
}

- (BOOL)routeTrackingEvent:(NSEvent*)event {
  MomoPlatformViews* views = _views;
  return event.trackingArea != nil && [views platformViewOwningTrackingArea:event.trackingArea] != 0;
}

@end
