/** 手势常量(逻辑像素 / 毫秒), 与 Flutter 一致。 */
export const kTouchSlop = 18;
export const kPanSlop = kTouchSlop * 2;
export const kPrecisePointerHitSlop = 1;
export const kPrecisePointerPanSlop = kPrecisePointerHitSlop * 2;
export const kDoubleTapTimeout = 300;
export const kDoubleTapSlop = 100;
export const kDoubleTapTouchSlop = kTouchSlop;
export const kPressTimeout = 100;
export const kLongPressTimeout = 500;
export const kMinFlingVelocity = 50;
export const kMaxFlingVelocity = 8000;
