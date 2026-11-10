#include "embedder/macos/mac_mouse_events.h"

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
