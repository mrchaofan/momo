#include "embedder/macos/mac_mouse_events.h"

using momo::embedder::PointerInput;
using momo::embedder::PointerWindow;
using momo::engine::platform::PointerData;
using momo::engine::platform::PointerDeviceKind;
using momo::engine::platform::PointerGestureKind;
using momo::engine::platform::SignalPhase;

MomoMouseEventKind MomoClassifyMouseEvent(NSEvent* event) {
  switch (event.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown:
      return MomoMouseEventKindDown;
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp:
      return MomoMouseEventKindUp;
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
      return MomoMouseEventKindDrag;
    case NSEventTypeMouseMoved:
      return MomoMouseEventKindMove;
    case NSEventTypeScrollWheel:
      return MomoMouseEventKindScroll;
    case NSEventTypeMouseEntered:
      return MomoMouseEventKindEnter;
    case NSEventTypeMouseExited:
      return MomoMouseEventKindExit;
    case NSEventTypeMagnify:
    case NSEventTypeRotate:
    case NSEventTypeSwipe:
    case NSEventTypeSmartMagnify:
    case NSEventTypeQuickLook:
    case NSEventTypePressure:
    case NSEventTypeTabletPoint:
    case NSEventTypeBeginGesture:
    case NSEventTypeEndGesture:
      return MomoMouseEventKindGesture;
    default:
      return MomoMouseEventKindNone;
  }
}

BOOL MomoTrackingAreaIsActive(NSTrackingArea* area, NSView* view) {
  const NSTrackingAreaOptions o = area.options;
  NSWindow* window = view.window;
  if (o & NSTrackingActiveAlways) return YES;
  if ((o & NSTrackingActiveInActiveApp) && NSApp.isActive) return YES;
  if ((o & NSTrackingActiveInKeyWindow) && window.isKeyWindow) return YES;
  if ((o & NSTrackingActiveWhenFirstResponder) && window.firstResponder == view) return YES;
  return NO;
}

NSRect MomoTrackingRect(NSTrackingArea* area, NSView* view) {
  return (area.options & NSTrackingInVisibleRect) ? view.visibleRect : area.rect;
}

BOOL MomoDeliverMouseMoved(NSView* root, NSEvent* event, NSView* skip) {
  BOOL delivered = NO;
  NSMutableArray<NSView*>* pending = [NSMutableArray arrayWithObject:root];
  NSHashTable* owners = [NSHashTable hashTableWithOptions:NSPointerFunctionsObjectPointerPersonality];
  while (pending.count > 0) {
    NSView* view = pending.lastObject;
    [pending removeLastObject];
    if (view == skip || view.hidden) continue;
    const NSPoint p = [view convertPoint:event.locationInWindow fromView:nil];
    for (NSTrackingArea* area in view.trackingAreas) {
      if ((area.options & NSTrackingMouseMoved) == 0 || !MomoTrackingAreaIsActive(area, view)) continue;
      id owner = area.owner;
      if (!NSPointInRect(p, MomoTrackingRect(area, view)) || [owners containsObject:owner] ||
          ![owner respondsToSelector:@selector(mouseMoved:)]) {
        continue;
      }
      [owners addObject:owner];  // 多个跟踪区共用 owner 时只投递一次
      [owner mouseMoved:event];
      delivered = YES;
    }
    [pending addObjectsFromArray:view.subviews];
  }
  return delivered;
}

namespace momo::embedder::macos {

namespace {

// 非精细滚动(普通滚轮)每行对应的逻辑像素(与 Chromium 一致)。
constexpr double kScrollLinePixels = 40.0;

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

// MomoMouseEventKindGesture 的手势字段。返回 false: 不处理的事件。
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

}  // namespace

CGFloat PrimaryScreenTop() {
  NSScreen* primary = NSScreen.screens.firstObject;
  return primary != nil ? NSMaxY(primary.frame) : 0;
}

NSPoint FlipScreenY(NSPoint point) { return NSMakePoint(point.x, PrimaryScreenTop() - point.y); }

Rect ScreenRectFromCocoa(NSRect rect) {
  return {rect.origin.x, PrimaryScreenTop() - NSMaxY(rect), rect.size.width, rect.size.height};
}

MacMouseInput::MacMouseInput() : windows_([NSHashTable weakObjectsHashTable]) {
  const NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown |
                           NSEventMaskOtherMouseDown | NSEventMaskLeftMouseDragged |
                           NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
                           NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp |
                           NSEventMaskOtherMouseUp;
  MacMouseInput* self = this;  // 监视器在析构时移除
  sequence_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                            handler:^NSEvent*(NSEvent* event) {
                                                              return self->RouteSequenceEvent(event) ? nil : event;
                                                            }];
}

MacMouseInput::~MacMouseInput() { [NSEvent removeMonitor:sequence_monitor_]; }

void MacMouseInput::AddWindow(NSWindow* window) { [windows_ addObject:window]; }

void MacMouseInput::RemoveWindow(NSWindow* window) { [windows_ removeObject:window]; }

void MacMouseInput::Dispatch(NSEvent* event, PointerWindow* window) {
  PointerInput input;
  if (Translate(event, window, input)) {
    dispatcher_.Dispatch(input);
  }
}

bool MacMouseInput::RouteSequenceEvent(NSEvent* event) {
  if (!dispatcher_.has_sequence() || MomoClassifyMouseEvent(event) == MomoMouseEventKindNone ||
      [windows_ containsObject:event.window]) {
    return false;
  }
  Dispatch(event, nullptr);
  return true;
}

uint32_t MacMouseInput::LogicalButton(NSEvent* event) {
  const uint32_t bit = ButtonBit(event);
  if (bit != 1u) return bit;
  if (event.type == NSEventTypeLeftMouseDown) {
    control_click_ = (event.modifierFlags & NSEventModifierFlagControl) != 0;
  }
  const bool control = control_click_;
  if (event.type == NSEventTypeLeftMouseUp) control_click_ = false;
  return control ? (1u << 1) : bit;
}

// NSEvent → PointerInput(只翻译, 不做决定)。返回 false: 不是指针输入。
bool MacMouseInput::Translate(NSEvent* event, PointerWindow* window, PointerInput& input) {
  using Type = PointerInput::Type;
  input.window = window;
  input.native = (__bridge void*)event;
  PointerData& data = input.data;
  data.timestamp_us = static_cast<int64_t>(event.timestamp * 1e6);
  data.kind = PointerDeviceKind::kMouse;
  const NSPoint screen = ScreenLocation(event);
  data.screen_x = screen.x;
  data.screen_y = screen.y;
  const NSUInteger pressed = [NSEvent pressedMouseButtons];
  const MomoMouseEventKind kind = MomoClassifyMouseEvent(event);
  switch (kind) {
    case MomoMouseEventKindDown:
    case MomoMouseEventKindUp: {
      input.type = kind == MomoMouseEventKindDown ? Type::kDown : Type::kUp;
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

}  // namespace momo::embedder::macos
