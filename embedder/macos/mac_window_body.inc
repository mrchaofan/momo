// MomoWindow / MomoPanel 共用的实现(在两个类的 @implementation 内各 #include 一次)。
// 两个类分别继承 NSWindow 与 NSPanel(面板需要 NSPanel 才能使用 NSWindowStyleMaskNonactivatingPanel),
// 鼠标派发、窗口控件等行为完全一致。所需实例变量: BOOL _systemTracking;int64_t _routedPlatformView。
// 本文件不是独立的编译单元。

@synthesize momoContentView, canBecomeMain, inputRouter;

// 面板(canBecomeMain = NO): 可接收键盘(key), 但不成为 main——应用主窗口保持 main 状态。
- (BOOL)canBecomeKeyWindow {
  return YES;
}
- (BOOL)canBecomeMainWindow {
  return self.canBecomeMain && [super canBecomeMainWindow];
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

// ---- MomoMouseRouting: 框架决定交给平台视图的事件经 AppKit 的正常投递交给它 ----

- (int64_t)momoRoutedPlatformView {
  return _routedPlatformView;
}

// 直接调用父类实现(不经下方的 sendEvent: 分流): 投递期间宿主的命中测试只落在该平台视图的原生视图子树里,
// AppKit 照常完成按下视图的捕获、第一响应者、手势识别器、immediate action 等。可重入(原生视图在处理中
// 跑嵌套事件循环时, 循环里的事件经 sendEvent: 重新进入 momo 的路线)。
- (void)momoDeliverEvent:(NSEvent*)event toPlatformView:(int64_t)viewId {
  const int64_t previous = _routedPlatformView;
  _routedPlatformView = viewId;
  [super sendEvent:event];
  _routedPlatformView = previous;
}

// 鼠标与其他位置型输入的入口(docs/05 §9 "输入"): 交给内容视图翻译后由 PointerDispatcher 决定接收方(momo
// 内容或平台视图), 不经 AppKit 投递。只有窗口装饰(红绿灯、边缘缩放区)的按下序列与它们自己的跟踪区事件
// 直接交给 AppKit。窗口拖动由应用在按下时请求(momo/window startDrag), MacWindow::StartWindowDrag 以该序列的
// 按下开始系统拖动。
- (void)sendEvent:(NSEvent*)event {
  MomoContentView* content = (MomoContentView*)self.momoContentView;
  const MomoMouseEventKind kind = MomoClassifyMouseEvent(event);
  // 跟踪区事件(进入/离开/光标更新)按归属分三类:
  //   原生视图(平台视图)的 → 不投递, 原生视图的跟踪状态由有序输入流推导(MomoPlatformViewInputRouter);
  //   momo 内容视图的 → 内容视图;
  //   窗口装饰(主题框架、标题栏、红绿灯)的 → AppKit, 它们维护窗口级状态, 不参与归属。
  const BOOL tracking = event.type == NSEventTypeCursorUpdate || kind == MomoMouseEventKindEnter ||
                        kind == MomoMouseEventKindExit;
  if (tracking && event.trackingArea != nil && [self.inputRouter routeTrackingEvent:event]) {
    return;
  }
  if (content == nil || kind == MomoMouseEventKindNone) {
    [super sendEvent:event];
    return;
  }
  // 窗口控件(红绿灯)优先于 momo: 整个按下序列交给 AppKit。
  if (_systemTracking) {
    [super sendEvent:event];
    if (kind == MomoMouseEventKindUp) _systemTracking = NO;
    return;
  }
  if (kind == MomoMouseEventKindDown && !MomoPointerSequenceActive()) {
    if (event.type == NSEventTypeLeftMouseDown && [self hitsWindowControl:[self frameHit:event]]) {
      _systemTracking = YES;
      [super sendEvent:event];
      return;
    }
    if (event.type == NSEventTypeLeftMouseDown && [content eventInSystemResizeZone:event]) {
      // 边缘缩放区(光标在这里由系统显示为缩放形状): 先交给 AppKit, 优先于 momo 内容。AppKit 在
      // -[NSWindow sendEvent:] 内跑缩放的跟踪循环直到松开;是否真的是缩放由它决定——松开后按键已不再按下,
      // 说明它接管了整个按下序列。否则(不在 AppKit 的缩放区)照常往下, 交给 momo。
      // 试探期间命中测试不落到任何平台视图(kNoPlatformView), 第一响应者保持不变: AppKit 若不缩放, 会把按下
      // 投递给命中的视图——不能让原生视图或焦点绕过 momo 的路线。
      NSResponder* responder = self.firstResponder;
      const int64_t routed = _routedPlatformView;
      _routedPlatformView = kNoPlatformView;
      [super sendEvent:event];
      _routedPlatformView = routed;
      if (([NSEvent pressedMouseButtons] & 1) == 0) {
        return;
      }
      if (self.firstResponder != responder) [self makeFirstResponder:responder];
    }
  }
  if (kind == MomoMouseEventKindEnter || kind == MomoMouseEventKindExit) {
    if (event.trackingArea == nil || event.trackingArea.owner != content) {
      [super sendEvent:event];  // 窗口装饰自己的跟踪区域(原生视图的已在上面处理)
      return;
    }
    [content handleMouseEvent:event];
    return;
  }
  // 交给 momo(先记录事件到达时窗口是否为 key), 再完成点击激活: 按下不经 AppKit, 窗口激活由这里完成
  // (面板只成为 key, 不激活应用)。
  [content handleMouseEvent:event];
  if (kind == MomoMouseEventKindDown) {
    [self activateForMomoClick];
    return;
  }
  // 移动: 窗口装饰照常记账;光标归属: 装饰在处理中设置了光标(缩放区)则归装饰,
  // 否则重新应用 momo 的光标(指针归平台视图时由原生视图管理, 见 refreshCursor)。
  if (kind == MomoMouseEventKindMove) {
    const BOOL chromeOwnsCursor = [self forwardMoveToWindowChrome:event];
    if (!chromeOwnsCursor) [content refreshCursor];
  }
}
