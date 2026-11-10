#include "embedder/macos/mac_mouse_dispatcher.h"

namespace {

NSUInteger ButtonMask(NSEvent* event) {
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
      const NSInteger n = event.buttonNumber;
      return n >= 2 && n < 32 ? (1u << n) : (1u << 2);
    }
  }
}

}  // namespace

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
    default:
      return MomoMouseEventKindNone;
  }
}

@implementation MomoMouseEventDispatcher {
  id<MomoMouseEventTarget> __weak _capture;
  id<MomoMouseEventTarget> __weak _hover;
  id<MomoMouseEventTarget> __weak _scroll;
  id<MomoMouseEventTarget> __weak _routed;
  NSUInteger _buttons;
  BOOL _routing;
}

- (id<MomoMouseEventTarget>)captureTarget {
  return _capture;
}
- (id<MomoMouseEventTarget>)hoverTarget {
  return _hover;
}
- (id<MomoMouseEventTarget>)scrollTarget {
  return _scroll;
}
- (id<MomoMouseEventTarget>)routedTarget {
  return _routed;
}
- (BOOL)routing {
  return _routing;
}
- (NSUInteger)pressedButtons {
  return _buttons;
}

- (void)beginRoutingToTarget:(id<MomoMouseEventTarget>)target {
  _routing = YES;
  _routed = target;
}

- (void)endRouting {
  _routing = NO;
  _routed = nil;
}

- (void)forgetTarget:(id<MomoMouseEventTarget>)target {
  if (_capture == target) {
    _capture = nil;
    _buttons = 0;
  }
  if (_hover == target) _hover = nil;
  if (_scroll == target) _scroll = nil;
  if (_routed == target) _routed = nil;
}

// 自上而下询问, 第一个消费者胜出。
- (id<MomoMouseEventTarget>)hitTest:(NSEvent*)event {
  NSArray<id<MomoMouseEventTarget>>* targets = self.targetsProvider ? self.targetsProvider() : @[];
  for (id<MomoMouseEventTarget> target in targets) {
    if ([target momoHandleMouseEvent:event captured:NO]) {
      return target;
    }
  }
  return nil;
}

- (void)setHover:(id<MomoMouseEventTarget>)target event:(NSEvent*)event {
  id<MomoMouseEventTarget> old = _hover;
  if (old == target) {
    return;
  }
  _hover = target;
  if (old != nil && [old respondsToSelector:@selector(momoMouseLeft:)]) {
    [old momoMouseLeft:event];
  }
  if (target != nil && [target respondsToSelector:@selector(momoMouseEntered:)]) {
    [target momoMouseEntered:event];
  }
}

- (id<MomoMouseEventTarget>)dispatchEvent:(NSEvent*)event {
  switch (MomoClassifyMouseEvent(event)) {
    case MomoMouseEventKindDown: {
      id<MomoMouseEventTarget> capture = _capture;
      if (capture != nil && _buttons != 0) {
        // 之前按下的按钮其实早已抬起(抬起事件被原生菜单等嵌套事件循环吞掉): 丢弃旧捕获, 重新命中测试。
        const NSUInteger pressed = self.pressedButtonsProvider ? self.pressedButtonsProvider()
                                                               : [NSEvent pressedMouseButtons];
        if ((pressed & _buttons & ~ButtonMask(event)) == 0) {
          _capture = nil;
          _buttons = 0;
          capture = nil;
        }
      }
      if (capture != nil && _buttons != 0) {
        // 另一个按钮在捕获期间按下: 仍属于同一指针序列。
        _buttons |= ButtonMask(event);
        [capture momoHandleMouseEvent:event captured:YES];
        return capture;
      }
      id<MomoMouseEventTarget> target = [self hitTest:event];
      _capture = target;
      _buttons = target != nil ? ButtonMask(event) : 0;
      [self setHover:target event:event];
      return target;
    }
    case MomoMouseEventKindDrag: {
      id<MomoMouseEventTarget> capture = _capture;
      if (capture != nil) {
        [capture momoHandleMouseEvent:event captured:YES];
        return capture;
      }
      // 没有捕获(按下发生在窗口外/标题栏): 当作移动。
      id<MomoMouseEventTarget> target = [self hitTest:event];
      [self setHover:target event:event];
      return target;
    }
    case MomoMouseEventKindUp: {
      id<MomoMouseEventTarget> capture = _capture;
      if (capture == nil) {
        return [self hitTest:event];
      }
      _buttons &= ~ButtonMask(event);
      [capture momoHandleMouseEvent:event captured:YES];
      if (_buttons == 0) {
        _capture = nil;  // 序列结束;下一次移动重新命中测试并更新悬停
      }
      return capture;
    }
    case MomoMouseEventKindMove:
    case MomoMouseEventKindEnter: {
      id<MomoMouseEventTarget> capture = _capture;
      if (capture != nil) {
        // 捕获期间(理论上 AppKit 此时只发拖拽)仍交给捕获者。
        [capture momoHandleMouseEvent:event captured:YES];
        return capture;
      }
      id<MomoMouseEventTarget> target = [self hitTest:event];
      [self setHover:target event:event];
      return target;
    }
    case MomoMouseEventKindExit: {
      if (_capture == nil) {
        [self setHover:nil event:event];
      }
      return nil;
    }
    case MomoMouseEventKindScroll: {
      // 触控板手势: Began / MayBegin 开启新手势(重新命中测试);之后的 Changed/Ended 与惯性阶段锁定。
      const NSEventPhase phase = event.phase;
      const NSEventPhase momentum = event.momentumPhase;
      const BOOL gesture = phase != NSEventPhaseNone || momentum != NSEventPhaseNone;
      const BOOL starts = (phase & (NSEventPhaseBegan | NSEventPhaseMayBegin)) != 0;
      id<MomoMouseEventTarget> latched = _scroll;
      id<MomoMouseEventTarget> target = nil;
      if (gesture && !starts && latched != nil) {
        [latched momoHandleMouseEvent:event captured:YES];
        target = latched;
      } else {
        target = [self hitTest:event];
        _scroll = gesture ? target : nil;
      }
      if ((momentum & (NSEventPhaseEnded | NSEventPhaseCancelled)) != 0 ||
          (phase & NSEventPhaseCancelled) != 0) {
        _scroll = nil;  // 惯性结束/手势取消: 解除锁定
      }
      return target;
    }
    case MomoMouseEventKindNone:
      return nil;
  }
  return nil;
}

@end
