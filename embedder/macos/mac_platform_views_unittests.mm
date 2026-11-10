// 平台视图的指针归属(mac_platform_views.h routeTrackingEvent:): 原生视图自己跟踪区的进入/离开
// 由派发器的悬停归属推导——被 momo 内容盖住时不投递进入、补发离开;归属恢复时补发进入。

#import <Cocoa/Cocoa.h>

#include <string>
#include <vector>

#include "embedder/macos/mac_platform_views.h"
#include "gtest/gtest.h"

// 假原生视图: 记录收到的跟踪事件(WKWebView 据此维护 :hover 等悬停状态)。
@interface FakeNativeView : NSView
@property(nonatomic) std::vector<std::string>* log;
@property(nonatomic, strong) NSTrackingArea* area;
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
    ASSERT_EQ(views_.mouseTargetsTopDown.count, 1u);
    container_ = views_.mouseTargetsTopDown[0];
  }

  Log log_;
  NSView* content_;
  MomoPlatformViews* views_;
  FakeNativeView* native_;
  id<MomoMouseEventTarget> container_;
};

// 指针归原生视图时: AppKit 的进入/离开/光标更新照常投递。
TEST_F(PlatformViewTrackingTest, OwnedPointerDeliversTrackingEvents) {
  [container_ momoMouseEntered:Crossing(NSEventTypeMouseEntered, native_.area)];
  EXPECT_TRUE([views_ routeTrackingEvent:Crossing(NSEventTypeMouseEntered, native_.area)]);
  EXPECT_TRUE([views_ routeTrackingEvent:Crossing(NSEventTypeCursorUpdate, native_.area)]);
  EXPECT_TRUE([views_ routeTrackingEvent:Crossing(NSEventTypeMouseExited, native_.area)]);
  EXPECT_EQ(log_, (Log{"entered", "cursorUpdate", "exited"}));
}

// 指针在原生视图的几何范围内、但被 momo 内容盖住(归属不在它): 进入与光标更新不投递;
// 盖住它的内容消失、归属回到它时补发进入;离开只发给收到过进入的跟踪区。
TEST_F(PlatformViewTrackingTest, OccludedPointerDefersEnterUntilOwnershipReturns) {
  EXPECT_TRUE([views_ routeTrackingEvent:Crossing(NSEventTypeMouseEntered, native_.area)]);
  EXPECT_TRUE([views_ routeTrackingEvent:Crossing(NSEventTypeCursorUpdate, native_.area)]);
  EXPECT_EQ(log_, Log{}) << "the pointer belongs to momo content above the view";
  [container_ momoMouseEntered:Crossing(NSEventTypeMouseEntered, native_.area)];
  EXPECT_EQ(log_, (Log{"entered"})) << "deferred enter replayed with its own tracking area";
  [container_ momoMouseEntered:Crossing(NSEventTypeMouseEntered, native_.area)];
  EXPECT_EQ(log_, (Log{"entered"})) << "no duplicate enter";
}

// 已在原生视图里, momo 内容(菜单)弹出盖住指针: 补发离开(清除悬停);AppKit 之后的真实离开不再重复。
TEST_F(PlatformViewTrackingTest, LosingOwnershipSynthesizesExitOnce) {
  [container_ momoMouseEntered:Crossing(NSEventTypeMouseEntered, native_.area)];
  [views_ routeTrackingEvent:Crossing(NSEventTypeMouseEntered, native_.area)];
  [container_ momoMouseLeft:Crossing(NSEventTypeMouseExited, native_.area)];
  EXPECT_EQ(log_, (Log{"entered", "exited"}));
  [views_ routeTrackingEvent:Crossing(NSEventTypeMouseExited, native_.area)];
  EXPECT_EQ(log_, (Log{"entered", "exited"})) << "the real exit after a synthesized one is dropped";
  // 菜单关闭、指针仍在视图上: 但 AppKit 认为已离开, 不再有可补发的进入(等它的下一次进入)。
  [container_ momoMouseEntered:Crossing(NSEventTypeMouseEntered, native_.area)];
  EXPECT_EQ(log_, (Log{"entered", "exited"}));
}

// 窗口装饰、momo 内容等不属于平台视图的跟踪区不经这里处理。
TEST_F(PlatformViewTrackingTest, ForeignTrackingAreasAreNotRouted) {
  NSTrackingArea* chrome = [[NSTrackingArea alloc] initWithRect:content_.bounds
                                                        options:NSTrackingMouseEnteredAndExited |
                                                                NSTrackingActiveAlways
                                                          owner:content_
                                                       userInfo:nil];
  [content_ addTrackingArea:chrome];
  EXPECT_FALSE([views_ routeTrackingEvent:Crossing(NSEventTypeMouseEntered, chrome)]);
  EXPECT_EQ(log_, Log{});
}

}  // namespace
