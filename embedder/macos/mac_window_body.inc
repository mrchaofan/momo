// MomoWindow / MomoPanel 共用的实现(在两个类的 @implementation 内各 #include 一次)。
// 两个类分别继承 NSWindow 与 NSPanel(面板需要 NSPanel 才能使用 NSWindowStyleMaskNonactivatingPanel),
// 鼠标派发、拖动区域、窗口控件等行为完全一致。所需实例变量: BOOL _systemTracking, _momoOnly。
// 本文件不是独立的编译单元。

@synthesize mouseDispatcher, momoContentView, dragRegions, canBecomeMain, platformViews;

// 面板(canBecomeMain = NO): 可接收键盘(key), 但不成为 main——应用主窗口保持 main 状态。
- (BOOL)canBecomeKeyWindow {
  return YES;
}
- (BOOL)canBecomeMainWindow {
  return self.canBecomeMain && [super canBecomeMainWindow];
}

- (BOOL)inDragRegion:(NSEvent*)event {
  NSView* content = self.momoContentView;
  if (content == nil || dragRegions.empty()) return NO;
  const NSPoint p = [content convertPoint:event.locationInWindow fromView:nil];
  const NSPoint top_left = NSMakePoint(p.x, content.bounds.size.height - p.y);
  for (const NSRect& r : dragRegions) {
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

// 跟踪区当前是否生效(按其激活条件)。
- (BOOL)trackingAreaIsActive:(NSTrackingArea*)area view:(NSView*)view {
  const NSTrackingAreaOptions o = area.options;
  if (o & NSTrackingActiveAlways) return YES;
  if ((o & NSTrackingActiveInActiveApp) && NSApp.isActive) return YES;
  if ((o & NSTrackingActiveInKeyWindow) && self.isKeyWindow) return YES;
  if ((o & NSTrackingActiveWhenFirstResponder) && self.firstResponder == view) return YES;
  return NO;
}

// 窗口装饰(主题框架、标题栏、红绿灯等内容视图之外的视图)的跟踪区: 不论事件由谁消费都照常收到移动。
// 它们维护窗口级状态而不是消费事件——典型是边缘缩放光标: 主题框架靠移动得知指针离开了缩放区,
// 才把光标交还给应用;收不到移动时窗口服务器会一直显示缩放光标并忽略应用设置的光标。
// 只转给装饰自己的跟踪区, 不经 AppKit 的全局投递, 所以平台视图(网页)收不到不属于它的移动。
// 返回装饰是否在处理中改变了当前光标(即装饰此刻拥有光标, 如位于缩放区)。
- (BOOL)forwardMoveToWindowChrome:(NSEvent*)event {
  NSView* content = self.momoContentView;
  NSView* frame = content.superview;
  if (frame == nil) return NO;
  NSCursor* before = NSCursor.currentCursor;
  NSMutableArray<NSView*>* pending = [NSMutableArray arrayWithObject:frame];
  while (pending.count > 0) {
    NSView* view = pending.lastObject;
    [pending removeLastObject];
    if (view == content || view.hiddenOrHasHiddenAncestor) continue;  // 内容子树归派发器
    const NSPoint p = [view convertPoint:event.locationInWindow fromView:nil];
    for (NSTrackingArea* area in view.trackingAreas) {
      if ((area.options & NSTrackingMouseMoved) == 0 || ![self trackingAreaIsActive:area view:view]) continue;
      const NSRect rect = (area.options & NSTrackingInVisibleRect) ? view.visibleRect : area.rect;
      if (NSPointInRect(p, rect) && [area.owner respondsToSelector:@selector(mouseMoved:)]) {
        [area.owner mouseMoved:event];
      }
    }
    [pending addObjectsFromArray:view.subviews];
  }
  return NSCursor.currentCursor != before;
}

// 不经 AppKit 处理的按下: 自行完成点击激活(面板只成为 key, 不激活应用)。
- (void)activateForMomoClick {
  if (!self.isKeyWindow && self.canBecomeKeyWindow) {
    if (self.canBecomeMain) {
      [self makeKeyAndOrderFront:nil];
      if (!NSApp.isActive) [NSApp activateIgnoringOtherApps:YES];
    } else {
      [self makeKeyWindow];
    }
  }
}

- (void)sendEvent:(NSEvent*)event {
  MomoMouseEventDispatcher* dispatcher = self.mouseDispatcher;
  const MomoMouseEventKind kind = MomoClassifyMouseEvent(event);
  // 跟踪区事件(进入/离开/光标更新)按归属分三类:
  //   原生视图(平台视图)的 → 按派发器的指针归属投递/暂存/补发(MomoPlatformViews routeTrackingEvent:);
  //   momo 内容视图的 → 派发器;
  //   窗口装饰(主题框架、标题栏、红绿灯)的 → AppKit, 它们维护窗口级状态, 不参与归属。
  const BOOL tracking = event.type == NSEventTypeCursorUpdate || kind == MomoMouseEventKindEnter ||
                        kind == MomoMouseEventKindExit;
  if (tracking && event.trackingArea != nil && [self.platformViews routeTrackingEvent:event]) {
    return;
  }
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
      [super sendEvent:event];  // 窗口装饰自己的跟踪区域(原生视图的已在上面处理)
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
    // 透明标题栏区域内的 momo 内容: 只由派发器处理;按下序列结束后恢复。按下不经 AppKit,
    // 窗口的点击激活由这里完成。
    if (kind == MomoMouseEventKindDown) [self activateForMomoClick];
    if (kind == MomoMouseEventKindUp && dispatcher.pressedButtons == 0) _momoOnly = NO;
    return;
  }
  if (via_appkit || button || target == nil) {
    [dispatcher beginRoutingToTarget:target];
    [super sendEvent:event];
    [dispatcher endRouting];
    return;
  }
  // 由 momo 消费、未交给 AppKit 的移动: 窗口装饰照常记账;光标归属: 装饰在处理中设置了光标(缩放区)
  // 则归装饰, 否则重新应用 momo 的光标(装饰交还控制权后, momo 的光标才真正生效)。
  if (kind == MomoMouseEventKindMove) {
    const BOOL chromeOwnsCursor = [self forwardMoveToWindowChrome:event];
    if (!chromeOwnsCursor && target == (id<MomoMouseEventTarget>)self.momoContentView) {
      [(MomoContentView*)self.momoContentView refreshCursor];
    }
  }
}
