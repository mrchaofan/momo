// 鼠标事件分类(mac_mouse_events.h)。

#import <Cocoa/Cocoa.h>

#include "embedder/macos/mac_mouse_events.h"
#include "gtest/gtest.h"

namespace {

NSEvent* Mouse(NSEventType type) {
  return [NSEvent mouseEventWithType:type
                            location:NSZeroPoint
                       modifierFlags:0
                           timestamp:0
                        windowNumber:0
                             context:nil
                         eventNumber:0
                          clickCount:1
                            pressure:0];
}

TEST(MouseEventsTest, ClassifiesEvents) {
  EXPECT_EQ(MomoClassifyMouseEvent(Mouse(NSEventTypeLeftMouseDown)), MomoMouseEventKindDown);
  EXPECT_EQ(MomoClassifyMouseEvent(Mouse(NSEventTypeRightMouseUp)), MomoMouseEventKindUp);
  EXPECT_EQ(MomoClassifyMouseEvent(Mouse(NSEventTypeOtherMouseDragged)), MomoMouseEventKindDrag);
  EXPECT_EQ(MomoClassifyMouseEvent(Mouse(NSEventTypeMouseMoved)), MomoMouseEventKindMove);
  EXPECT_EQ(MomoClassifyMouseEvent([NSEvent keyEventWithType:NSEventTypeKeyDown
                                                    location:NSZeroPoint
                                               modifierFlags:0
                                                   timestamp:0
                                                windowNumber:0
                                                     context:nil
                                                  characters:@"a"
                                 charactersIgnoringModifiers:@"a"
                                                   isARepeat:NO
                                                     keyCode:0]),
            MomoMouseEventKindNone);
}

}  // namespace
