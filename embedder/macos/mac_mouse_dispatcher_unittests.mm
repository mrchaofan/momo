// 鼠标事件派发器(mac_mouse_dispatcher.h): 用假目标验证"第一个返回 YES 的目标独占事件"及捕获/锁定规则。

#import <Cocoa/Cocoa.h>

#include <string>
#include <vector>

#include "embedder/macos/mac_mouse_dispatcher.h"
#include "gtest/gtest.h"

@interface FakeMouseTarget : NSObject <MomoMouseEventTarget>
@property(nonatomic) BOOL consumes;
@property(nonatomic, copy) NSString* name;
@property(nonatomic) std::vector<std::string>* log;
@end

@implementation FakeMouseTarget
- (BOOL)momoHandleMouseEvent:(NSEvent*)event captured:(BOOL)captured {
  self.log->push_back(std::string(self.name.UTF8String) + (captured ? ":captured" : ":ask"));
  return captured || self.consumes;
}
- (void)momoMouseLeft:(NSEvent*)event {
  self.log->push_back(std::string(self.name.UTF8String) + ":left");
}
@end

namespace {

NSEvent* Mouse(NSEventType type, NSInteger button = 0) {
  NSEvent* e = [NSEvent mouseEventWithType:type
                                  location:NSMakePoint(10, 10)
                             modifierFlags:0
                                 timestamp:0
                              windowNumber:0
                                   context:nil
                               eventNumber:0
                                clickCount:1
                                  pressure:1];
  if (button >= 2) {
    // otherMouse 事件的 buttonNumber 由 CGEvent 字段给出。
    CGEventRef cg = CGEventCreateCopy(e.CGEvent);
    CGEventSetIntegerValueField(cg, kCGMouseEventButtonNumber, button);
    e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
  }
  return e;
}

NSEvent* Exited() {
  return [NSEvent enterExitEventWithType:NSEventTypeMouseExited
                                location:NSMakePoint(10, 10)
                           modifierFlags:0
                               timestamp:0
                            windowNumber:0
                                 context:nil
                             eventNumber:0
                          trackingNumber:0
                                userData:nullptr];
}

// phase / momentum 取 CGEvent 字段值: phase Began=1 Changed=2 Ended=4 Cancelled=8 MayBegin=128;
// momentum Begin=1 Continue=2 End=3。
NSEvent* Scroll(int64_t phase, int64_t momentum) {
  CGEventRef cg = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 1, -5);
  CGEventSetIntegerValueField(cg, kCGScrollWheelEventScrollPhase, phase);
  CGEventSetIntegerValueField(cg, kCGScrollWheelEventMomentumPhase, momentum);
  NSEvent* e = [NSEvent eventWithCGEvent:cg];
  CFRelease(cg);
  return e;
}

class MouseDispatcherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    top_ = Make(@"top", YES);
    bottom_ = Make(@"bottom", YES);
    dispatcher_ = [[MomoMouseEventDispatcher alloc] init];
    dispatcher_.pressedButtonsProvider = ^NSUInteger { return pressed_; };
    FakeMouseTarget* top = top_;
    FakeMouseTarget* bottom = bottom_;
    dispatcher_.targetsProvider = ^NSArray<id<MomoMouseEventTarget>>* {
      return @[ top, bottom ];
    };
  }
  FakeMouseTarget* Make(NSString* name, BOOL consumes) {
    FakeMouseTarget* t = [[FakeMouseTarget alloc] init];
    t.name = name;
    t.consumes = consumes;
    t.log = &log_;
    return t;
  }
  id<MomoMouseEventTarget> Dispatch(NSEvent* e) { return [dispatcher_ dispatchEvent:e]; }

  std::vector<std::string> log_;
  NSUInteger pressed_ = 0xFF;  // 缺省: 认为之前的按钮都还按着
  FakeMouseTarget* top_;
  FakeMouseTarget* bottom_;
  MomoMouseEventDispatcher* dispatcher_;
};

using Log = std::vector<std::string>;

TEST_F(MouseDispatcherTest, ClassifiesEvents) {
  EXPECT_EQ(MomoClassifyMouseEvent(Mouse(NSEventTypeLeftMouseDown)), MomoMouseEventKindDown);
  EXPECT_EQ(MomoClassifyMouseEvent(Mouse(NSEventTypeRightMouseUp)), MomoMouseEventKindUp);
  EXPECT_EQ(MomoClassifyMouseEvent(Mouse(NSEventTypeOtherMouseDragged, 2)), MomoMouseEventKindDrag);
  EXPECT_EQ(MomoClassifyMouseEvent(Mouse(NSEventTypeMouseMoved)), MomoMouseEventKindMove);
  EXPECT_EQ(MomoClassifyMouseEvent(Scroll(0, 0)), MomoMouseEventKindScroll);
  EXPECT_EQ(MomoClassifyMouseEvent(Exited()), MomoMouseEventKindExit);
}

// 第一个返回 YES 的目标消费事件, 之后的目标不再被询问。
TEST_F(MouseDispatcherTest, TopmostConsumerWinsExclusively) {
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeMouseMoved)), top_);
  EXPECT_EQ(log_, (Log{"top:ask"}));
}

TEST_F(MouseDispatcherTest, DeclinedEventsFallThroughInZOrder) {
  top_.consumes = NO;
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeLeftMouseDown)), bottom_);
  EXPECT_EQ(log_, (Log{"top:ask", "bottom:ask"}));
  bottom_.consumes = NO;
  log_.clear();
  [dispatcher_ forgetTarget:bottom_];
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeMouseMoved)), nil);
}

// 按下即捕获: 拖拽、其他按钮、抬起都交给捕获者, 即使此刻上层目标想要它;全部按钮抬起后释放。
TEST_F(MouseDispatcherTest, PressCapturesTheSequenceAcrossButtons) {
  top_.consumes = NO;
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeLeftMouseDown)), bottom_);
  top_.consumes = YES;
  log_.clear();
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeLeftMouseDragged)), bottom_);
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeRightMouseDown)), bottom_);
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeLeftMouseUp)), bottom_);
  EXPECT_EQ(dispatcher_.captureTarget, bottom_) << "right button still down";
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeRightMouseUp)), bottom_);
  EXPECT_EQ(dispatcher_.captureTarget, nil);
  EXPECT_EQ(log_, (Log{"bottom:captured", "bottom:captured", "bottom:captured",
                       "bottom:captured"}));
  // 释放后重新命中测试;悬停从 bottom 移到 top, bottom 收到离开。
  log_.clear();
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeMouseMoved)), top_);
  EXPECT_EQ(log_, (Log{"top:ask", "bottom:left"}));
}

// 抬起事件被原生菜单吞掉: 下一次按下发现之前的按钮早已抬起, 丢弃旧捕获并重新命中测试。
TEST_F(MouseDispatcherTest, StaleCaptureIsDroppedWhenButtonsAreNoLongerPressed) {
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeRightMouseDown)), top_);  // 例如在网页上右键(原生菜单)
  // (rightMouseUp 没有经过派发器)
  pressed_ = 0;
  top_.consumes = NO;
  log_.clear();
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeRightMouseDown)), bottom_) << "fresh hit test, not the stale capture";
  EXPECT_EQ(log_, (Log{"top:ask", "bottom:ask", "top:left"}));
  EXPECT_EQ(dispatcher_.captureTarget, bottom_);
}

TEST_F(MouseDispatcherTest, HoverChangeAndWindowExitNotifyTheOldTarget) {
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeMouseMoved)), top_);
  top_.consumes = NO;
  log_.clear();
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeMouseMoved)), bottom_);
  EXPECT_EQ(log_, (Log{"top:ask", "bottom:ask", "top:left"}));
  log_.clear();
  EXPECT_EQ(Dispatch(Exited()), nil);
  EXPECT_EQ(log_, (Log{"bottom:left"}));
  EXPECT_EQ(dispatcher_.hoverTarget, nil);
}

// 触控板滚动手势锁定首个消费者, 直到惯性结束;新手势重新命中测试。普通滚轮每次命中测试。
TEST_F(MouseDispatcherTest, ScrollGesturesLatchUntilMomentumEnds) {
  EXPECT_EQ(Dispatch(Scroll(/*Began*/ 1, 0)), top_);
  top_.consumes = NO;
  EXPECT_EQ(Dispatch(Scroll(/*Changed*/ 2, 0)), top_);
  EXPECT_EQ(Dispatch(Scroll(/*Ended*/ 4, 0)), top_);
  EXPECT_EQ(Dispatch(Scroll(0, /*Begin*/ 1)), top_);
  EXPECT_EQ(Dispatch(Scroll(0, /*Continue*/ 2)), top_);
  EXPECT_EQ(Dispatch(Scroll(0, /*End*/ 3)), top_);
  EXPECT_EQ(dispatcher_.scrollTarget, nil);
  EXPECT_EQ(Dispatch(Scroll(/*Began*/ 1, 0)), bottom_);
  // 普通滚轮(无 phase): 不锁定。
  top_.consumes = YES;
  EXPECT_EQ(Dispatch(Scroll(0, 0)), top_);
  top_.consumes = NO;
  EXPECT_EQ(Dispatch(Scroll(0, 0)), bottom_);
  EXPECT_EQ(dispatcher_.scrollTarget, nil);
}

TEST_F(MouseDispatcherTest, RoutingStateAndForgottenCapture) {
  EXPECT_FALSE(dispatcher_.routing);
  [dispatcher_ beginRoutingToTarget:top_];
  EXPECT_TRUE(dispatcher_.routing);
  EXPECT_EQ(dispatcher_.routedTarget, top_);
  [dispatcher_ endRouting];
  EXPECT_FALSE(dispatcher_.routing);
  EXPECT_EQ(dispatcher_.routedTarget, nil);

  // 被捕获的目标消失(平台视图被移除): 捕获解除, 后续拖拽重新命中测试。
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeLeftMouseDown)), top_);
  [dispatcher_ forgetTarget:top_];
  EXPECT_EQ(dispatcher_.captureTarget, nil);
  top_.consumes = NO;
  EXPECT_EQ(Dispatch(Mouse(NSEventTypeLeftMouseDragged)), bottom_);
}

}  // namespace
