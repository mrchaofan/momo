// 平台视图的输入路由(mac_platform_view_input.h): 框架回送的事件经 AppKit 投递给选中的原生视图(移动投递给
// 跟踪区 owner);原生视图跟踪区的进入/离开/光标更新只由这条有序输入流(事件 + 框架的指针归属)推导,
// AppKit 自己的跟踪事件不投递。平台视图本身(MomoPlatformViews)只负责几何。

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <string>
#include <vector>

#include "embedder/macos/mac_platform_view_input.h"
#include "embedder/macos/mac_platform_views.h"
#include "gtest/gtest.h"

// 假原生视图: 记录收到的跟踪事件(WKWebView 据此维护 :hover 等悬停状态)。
@interface FakeNativeView : NSView
@property(nonatomic) std::vector<std::string>* log;
@property(nonatomic, strong) NSTrackingArea* area;
@property(nonatomic) BOOL refusesFirstMouse;
@end

@implementation FakeNativeView
- (void)mouseEntered:(NSEvent*)event {
  self.log->push_back(event.trackingArea == self.area ? "entered" : "entered(other area)");
}
- (void)mouseExited:(NSEvent*)event {
  self.log->push_back(event.trackingArea == self.area ? "exited" : "exited(other area)");
}
- (void)cursorUpdate:(NSEvent*)event {
  self.log->push_back("cursorUpdate");
}
- (void)mouseMoved:(NSEvent*)event {
  self.log->push_back("moved");
}
- (void)mouseDown:(NSEvent*)event {
  self.log->push_back("down");
}
- (void)mouseDragged:(NSEvent*)event {
  self.log->push_back("dragged");
}
- (void)mouseUp:(NSEvent*)event {
  self.log->push_back("up");
}
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return !self.refusesFirstMouse;
}
@end

// 测试窗口: 与 MomoWindow 相同, 把框架回送的事件经 AppKit 的正常投递(父类 sendEvent:)交给平台视图。
@interface RoutingTestWindow : NSWindow <MomoMouseRouting>
@end
@implementation RoutingTestWindow {
  int64_t _routed;
}
- (int64_t)momoRoutedPlatformView {
  return _routed;
}
- (void)momoDeliverEvent:(NSEvent*)event toPlatformView:(int64_t)viewId {
  const int64_t previous = _routed;
  _routed = viewId;
  [super sendEvent:event];
  _routed = previous;
}
@end

// 记录识别结果的目标。
@interface RecognizerTarget : NSObject
@property(nonatomic) std::vector<std::string>* log;
- (void)clicked:(NSGestureRecognizer*)recognizer;
@end
@implementation RecognizerTarget
- (void)clicked:(NSGestureRecognizer*)recognizer {
  if (recognizer.state == NSGestureRecognizerStateEnded) self.log->push_back("click recognized");
}
@end

@interface FakeMoveObserver : NSObject
@property(nonatomic) std::vector<std::string>* log;
@end
@implementation FakeMoveObserver
- (void)mouseMoved:(NSEvent*)event {
  self.log->push_back("observer moved");
}
@end

namespace {

using Log = std::vector<std::string>;

NSEvent* Crossing(NSEventType type, NSTrackingArea* area) {
  return [NSEvent enterExitEventWithType:type
                                location:NSMakePoint(50, 50)
                           modifierFlags:0
                               timestamp:0
                            windowNumber:0
                                 context:nil
                             eventNumber:0
                          trackingNumber:(NSInteger)(__bridge void*)area
                                userData:nullptr];
}

class PlatformViewTrackingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    content_ = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
    views_ = [[MomoPlatformViews alloc] initWithContentView:content_];
    router_ = [[MomoPlatformViewInputRouter alloc] initWithPlatformViews:views_ contentView:content_];
    native_ = [[FakeNativeView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    native_.log = &log_;
    native_.area = [[NSTrackingArea alloc] initWithRect:native_.bounds
                                                options:NSTrackingMouseEnteredAndExited |
                                                        NSTrackingCursorUpdate |
                                                        NSTrackingActiveAlways
                                                  owner:native_
                                               userInfo:nil];
    [native_ addTrackingArea:native_.area];
    [views_ addView:native_ viewId:7];
    momo::engine::platform::CompositionLayout layout;
    momo::engine::platform::PlatformViewGeometry g;
    g.view_id = 7;
    g.frame = {0, 0, 100, 100};
    layout.views.push_back(g);
    layout.overlays.push_back(std::nullopt);
    [views_ applyLayout:layout overlayLayers:{}];
  }

  Log log_;
  NSView* content_;
  MomoPlatformViews* views_;
  MomoPlatformViewInputRouter* router_;
  FakeNativeView* native_;
};

// ---- 宿主转发(容器即原生视图的 HostView)----

// 当前测试窗口的编号(事件须属于该窗口, AppKit 才会投递)。
NSInteger g_window_number = 0;

NSEvent* MouseAt(NSEventType type, NSPoint window_point, NSInteger window_number = -1) {
  if (window_number < 0) window_number = g_window_number;
  return [NSEvent mouseEventWithType:type
                            location:window_point
                       modifierFlags:0
                           timestamp:0
                        windowNumber:window_number
                             context:nil
                         eventNumber:0
                          clickCount:1
                            pressure:0];
}

RoutingTestWindow* MakeWindow(NSView* content) {
  // 放在屏幕范围之外: 窗口须在屏幕上(已排入)AppKit 才投递鼠标事件, 但不必可见。
  RoutingTestWindow* window = [[RoutingTestWindow alloc] initWithContentRect:NSMakeRect(-20000, -20000, 200, 200)
                                                                    styleMask:NSWindowStyleMaskBorderless
                                                                      backing:NSBackingStoreBuffered
                                                                        defer:NO];
  window.releasedWhenClosed = NO;
  [window.contentView addSubview:content];
  [NSApplication sharedApplication];
  [window orderFrontRegardless];  // AppKit 只为屏幕上的窗口投递鼠标事件
  g_window_number = window.windowNumber;
  return window;
}

// 窗口坐标 (x, 150) → 原生视图坐标 (x, 50)(见 MovesAreForwardedWithoutFocus)。
NSEvent* MoveAt(CGFloat x) {
  return MouseAt(NSEventTypeMouseMoved, NSMakePoint(x, 150));
}

// AppKit 为原生视图跟踪区生成的进入/离开/光标更新: 认领(不再交给 AppKit), 但不投递。
TEST_F(PlatformViewTrackingTest, AppKitTrackingEventsAreNotDelivered) {
  [router_ setPointerOwned:YES platformView:7 event:MoveAt(150)];  // 归属在, 但指针在原生视图之外
  EXPECT_TRUE([router_ routeTrackingEvent:Crossing(NSEventTypeMouseEntered, native_.area)]);
  EXPECT_TRUE([router_ routeTrackingEvent:Crossing(NSEventTypeCursorUpdate, native_.area)]);
  EXPECT_TRUE([router_ routeTrackingEvent:Crossing(NSEventTypeMouseExited, native_.area)]);
  EXPECT_EQ(log_, Log{});
}

// 归属驱动进入/离开: 进入时带光标更新;离开在触发事件的位置(如窗口外)投递。
TEST_F(PlatformViewTrackingTest, OwnershipDrivesEnterAndExit) {
  [router_ setPointerOwned:YES platformView:7 event:MoveAt(50)];
  EXPECT_EQ(log_, (Log{"entered", "cursorUpdate"}));
  [router_ deliverEvent:MoveAt(60) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"entered", "cursorUpdate", "moved"})) << "still inside: no extra crossing";
  [router_ setPointerOwned:NO platformView:7 event:MoveAt(250)];
  EXPECT_EQ(log_, (Log{"entered", "cursorUpdate", "moved", "exited"}));
}

// 回归: 指针从原生视图直接移出窗口。AppKit 的离开同步到达, 之前的移动经 ui 线程往返后才到——
// 若投递 AppKit 的离开, 原生视图会在离开之后又被移动带回悬停。现在离开只来自框架, 排在全部移动之后。
TEST_F(PlatformViewTrackingTest, LateMovesAfterAppKitExitDoNotLeaveTheViewHovered) {
  [router_ setPointerOwned:YES platformView:7 event:MoveAt(50)];
  [router_ routeTrackingEvent:Crossing(NSEventTypeMouseExited, native_.area)];  // AppKit: 已离开窗口
  [router_ deliverEvent:MoveAt(90) toPlatformView:7 windowWasKey:YES];                // 迟到的移动
  [router_ setPointerOwned:NO platformView:7 event:MoveAt(250)];                      // 框架: 离开(窗口外)
  EXPECT_EQ(log_, (Log{"entered", "cursorUpdate", "moved", "exited"}));
}

// 归属期间按几何更新原生视图内的各个跟踪区(网页内部可以有多个);归属离开时全部离开。
TEST_F(PlatformViewTrackingTest, TrackingAreasInsideTheViewFollowTheStream) {
  NSTrackingArea* left = [[NSTrackingArea alloc] initWithRect:NSMakeRect(0, 0, 50, 100)
                                                      options:NSTrackingMouseEnteredAndExited |
                                                              NSTrackingActiveAlways
                                                        owner:native_
                                                     userInfo:nil];
  [native_ addTrackingArea:left];
  // 同一事件引起的多个跟踪区进出之间没有规定的先后(取决于 trackingAreas 的顺序): 按集合比较。
  auto sorted = [](Log log) {
    std::sort(log.begin(), log.end());
    return log;
  };
  [router_ setPointerOwned:YES platformView:7 event:MoveAt(25)];
  EXPECT_EQ(sorted(log_), sorted(Log{"entered", "cursorUpdate", "entered(other area)"}));
  log_.clear();
  [router_ deliverEvent:MoveAt(75) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"exited(other area)", "moved"})) << "crossings precede the move, as in AppKit";
  log_.clear();
  [router_ deliverEvent:MoveAt(25) toPlatformView:7 windowWasKey:YES];
  [router_ setPointerOwned:NO platformView:7 event:MoveAt(250)];
  ASSERT_EQ(log_.size(), 4u);
  EXPECT_EQ((Log{log_[0], log_[1]}), (Log{"entered(other area)", "moved"}));
  EXPECT_EQ(sorted({log_[2], log_[3]}), sorted(Log{"exited", "exited(other area)"}));
  [native_ removeTrackingArea:left];
}

// 按下拖动期间: 不带 NSTrackingEnabledDuringMouseDrag 的跟踪区保持原状, 抬起后再更新(与 AppKit 相同)。
TEST_F(PlatformViewTrackingTest, DragDefersCrossingsUntilMouseUp) {
  NSWindow* window = MakeWindow(content_);
  [router_ setPointerOwned:YES platformView:7 event:MoveAt(50)];
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseDown, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:YES];
  log_.clear();
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseDragged, NSMakePoint(150, 150)) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"dragged"})) << "no exit while dragging out";
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseUp, NSMakePoint(150, 150)) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"dragged", "up", "exited"})) << "deferred exit after the button is released";
  [content_ removeFromSuperview];
  [window close];
}

// 窗口装饰、momo 内容等不属于平台视图的跟踪区不经这里处理。
TEST_F(PlatformViewTrackingTest, ForeignTrackingAreasAreNotRouted) {
  NSTrackingArea* chrome = [[NSTrackingArea alloc] initWithRect:content_.bounds
                                                        options:NSTrackingMouseEnteredAndExited |
                                                                NSTrackingActiveAlways
                                                          owner:content_
                                                       userInfo:nil];
  [content_ addTrackingArea:chrome];
  EXPECT_FALSE([router_ routeTrackingEvent:Crossing(NSEventTypeMouseEntered, chrome)]);
  EXPECT_EQ(log_, Log{});
}

// 框架决定交给平台视图的移动直接转发给原生视图: 不要求它是 first responder
// (AppKit 的移动只发给 first responder)。
TEST_F(PlatformViewTrackingTest, MovesAreForwardedWithoutFocus) {
  // content_ 未翻转、高 200: 宿主(翻转)坐标 (50,50) 对应窗口坐标 (50,150)。
  [router_ deliverEvent:MouseAt(NSEventTypeMouseMoved, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"moved"}));
  [router_ deliverEvent:MouseAt(NSEventTypeMouseMoved, NSMakePoint(50, 150)) toPlatformView:8 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"moved"})) << "unknown platform view: ignored";
}

// 跟踪区 owner 不是视图(WebKit 的内部观察者): 移动按 AppKit 的规则投递给 owner, 而不是命中视图的
// mouseMoved:(WKWebView 不实现 mouseMoved:, 悬停全靠跟踪区)。指针在跟踪区外时不投递。
TEST_F(PlatformViewTrackingTest, MovesGoToMouseMovedTrackingAreaOwners) {
  FakeMoveObserver* observer = [[FakeMoveObserver alloc] init];
  observer.log = &log_;
  NSTrackingArea* area = [[NSTrackingArea alloc] initWithRect:NSMakeRect(0, 0, 100, 50)
                                                      options:NSTrackingMouseMoved | NSTrackingActiveAlways
                                                        owner:observer
                                                     userInfo:nil];
  [native_ addTrackingArea:area];
  // 原生视图(未翻转)坐标 y ∈ [0,50) 对应宿主(翻转)y ∈ (50,100], 即窗口 y ∈ [100,150)。
  [router_ deliverEvent:MouseAt(NSEventTypeMouseMoved, NSMakePoint(50, 120)) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"observer moved"})) << "delivered to the tracking area owner only";
  log_.clear();
  [router_ deliverEvent:MouseAt(NSEventTypeMouseMoved, NSMakePoint(50, 180)) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"moved"})) << "outside every mouse-moved area: falls back to the hit view";
  [native_ removeTrackingArea:area];
}

// 框架回送的按钮事件经 AppKit 的正常投递: AppKit 在原生视图内命中、让它成为第一响应者;
// 拖出原生视图后, 拖拽/抬起仍发给收到按下的视图(AppKit 自己的按下捕获)。
TEST_F(PlatformViewTrackingTest, ButtonEventsAreDeliveredThroughAppKit) {
  NSWindow* window = MakeWindow(content_);
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseDown, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(window.firstResponder, native_);
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseDragged, NSMakePoint(180, 10)) toPlatformView:7 windowWasKey:YES];
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseUp, NSMakePoint(180, 10)) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"down", "dragged", "up"}));
  // 没有在本视图开始的按下序列的拖拽/抬起: 不投递。
  log_.clear();
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseDragged, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:YES];
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseUp, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, Log{});
  [content_ removeFromSuperview];
  [window close];
}

// 原生视图上的手势识别器由 AppKit 在投递中驱动(WKWebView 用力点按预览靠 NSImmediateActionGestureRecognizer)。
TEST_F(PlatformViewTrackingTest, GestureRecognizersOnTheNativeViewSeeRoutedEvents) {
  NSWindow* window = MakeWindow(content_);
  RecognizerTarget* target = [[RecognizerTarget alloc] init];
  target.log = &log_;
  NSClickGestureRecognizer* click = [[NSClickGestureRecognizer alloc] initWithTarget:target
                                                                              action:@selector(clicked:)];
  [native_ addGestureRecognizer:click];
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseDown, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:YES];
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseUp, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:YES];
  EXPECT_NE(std::find(log_.begin(), log_.end(), "click recognized"), log_.end()) << ::testing::PrintToString(log_);
  [native_ removeGestureRecognizer:click];
  [content_ removeFromSuperview];
  [window close];
}

// 非 key 窗口上的点击: 原生视图不接受 first mouse 时整个按下序列只用于激活窗口, 不投递。
TEST_F(PlatformViewTrackingTest, ClickThatOnlyActivatesTheWindowIsNotDelivered) {
  NSWindow* window = MakeWindow(content_);
  native_.refusesFirstMouse = YES;
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseDown, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:NO];
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseUp, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:NO];
  EXPECT_EQ(log_, Log{});
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseDown, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:YES];
  [router_ deliverEvent:MouseAt(NSEventTypeLeftMouseUp, NSMakePoint(50, 150)) toPlatformView:7 windowWasKey:YES];
  EXPECT_EQ(log_, (Log{"down", "up"})) << "the next click in the (now key) window is delivered";
  [content_ removeFromSuperview];
  [window close];
}

// AppKit 的几何命中("指针下是哪个视图")以框架的指针归属为准: WKWebView 在设置光标前用内容视图的
// hitTest: 确认指针下是它自己, 指针不归它时迟到的光标设置因此被它自己丢弃。框架不掌握指针位置时
// (拖放会话等)退回按画面上的遮挡近似。
TEST_F(PlatformViewTrackingTest, AppKitHitTestingFollowsPointerOwnership) {
  views_.inputRouter = router_;
  const NSPoint p = NSMakePoint(50, 150);  // content_ 坐标(无父视图: 即自身坐标)
  router_.pointerTracked = YES;
  EXPECT_NE([content_ hitTest:p], (NSView*)native_) << "pointer belongs to momo content";
  [router_ setPointerOwned:YES platformView:7 event:MoveAt(50)];
  EXPECT_EQ([content_ hitTest:p], (NSView*)native_);
  [router_ setPointerOwned:NO platformView:7 event:MoveAt(250)];
  EXPECT_NE([content_ hitTest:p], (NSView*)native_);
  router_.pointerTracked = NO;  // 例如外部拖放会话: 按几何
  EXPECT_EQ([content_ hitTest:p], (NSView*)native_);
}

// 指针归属由框架设置: pointerOwned 反映是否有平台视图持有指针(光标交给原生视图)。
TEST_F(PlatformViewTrackingTest, PointerOwnedFollowsFramework) {
  EXPECT_FALSE(router_.pointerOwned);
  [router_ setPointerOwned:YES platformView:7 event:nil];
  EXPECT_TRUE(router_.pointerOwned);
  [router_ setPointerOwned:NO platformView:7 event:nil];
  EXPECT_FALSE(router_.pointerOwned);
}

}  // namespace
