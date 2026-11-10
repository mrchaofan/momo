import type { Offset } from "../painting/geometry";
import type { DelayedTask } from "../scheduler/binding";
import { SchedulerBinding } from "../scheduler/binding";
import type { GestureArenaEntry } from "./arena";
import { GestureBinding } from "./binding";
import { kDoubleTapSlop, kDoubleTapTimeout, kDoubleTapTouchSlop } from "./constants";
import {
  PointerCancelEvent, PointerMoveEvent, PointerUpEvent, kPrimaryButton, type PointerDownEvent, type PointerEvent,
} from "./events";
import { GestureRecognizer } from "./recognizer";

interface TapTracker {
  pointer: number;
  entry: GestureArenaEntry;
  initial: Offset;
  route: (e: PointerEvent) => void;
  isTrackingPointer: boolean;
}

/**
 * 双击识别(对标 Flutter DoubleTapGestureRecognizer):第一次抬起后 hold 竞技场等待第二次按下(300ms 内,
 * 距离 100 内);成功则两场都接受并回调 onDoubleTap, 超时则放弃, 让单击得以获胜。
 */
export class DoubleTapGestureRecognizer extends GestureRecognizer {
  onDoubleTapDown: ((position: Offset) => void) | null = null;
  onDoubleTap: (() => void) | null = null;
  onDoubleTapCancel: (() => void) | null = null;

  #firstTap: TapTracker | null = null;
  #trackers = new Map<number, TapTracker>();
  #timer: DelayedTask | null = null;

  override isPointerAllowed(event: PointerDownEvent): boolean {
    if (this.#firstTap === null && event.buttons !== kPrimaryButton) return false;
    if (this.onDoubleTap === null && this.onDoubleTapDown === null && this.onDoubleTapCancel === null) return false;
    return super.isPointerAllowed(event);
  }

  protected addAllowedPointer(event: PointerDownEvent): void {
    if (this.#firstTap !== null) {
      if (event.position.minus(this.#firstTap.initial).distance > kDoubleTapSlop) return;
      this.#stopTimer();
      this.onDoubleTapDown?.(event.position);
    }
    this.#trackTap(event);
  }

  #trackTap(event: PointerDownEvent): void {
    const g = GestureBinding.instance;
    const tracker: TapTracker = {
      pointer: event.pointer,
      entry: g.gestureArena.add(event.pointer, this),
      initial: event.position,
      route: (e) => this.#handleEvent(e),
      isTrackingPointer: true,
    };
    this.#trackers.set(event.pointer, tracker);
    g.pointerRouter.addRoute(event.pointer, tracker.route, event.transform);
  }

  #handleEvent(event: PointerEvent): void {
    const tracker = this.#trackers.get(event.pointer);
    if (tracker === undefined) return;
    if (event instanceof PointerUpEvent) {
      if (this.#firstTap === null) this.#registerFirstTap(tracker);
      else this.#registerSecondTap(tracker);
    } else if (event instanceof PointerMoveEvent) {
      if (event.position.minus(tracker.initial).distance > kDoubleTapTouchSlop) this.#reject(tracker);
    } else if (event instanceof PointerCancelEvent) {
      this.#reject(tracker);
    }
  }

  override acceptGesture(_pointer: number): void {}

  override rejectGesture(pointer: number): void {
    let tracker = this.#trackers.get(pointer);
    if (tracker === undefined && this.#firstTap !== null && this.#firstTap.pointer === pointer) tracker = this.#firstTap;
    if (tracker !== undefined) this.#reject(tracker);
  }

  #reject(tracker: TapTracker): void {
    this.#trackers.delete(tracker.pointer);
    tracker.entry.resolve("rejected");
    this.#freeze(tracker);
    if (this.#firstTap !== null) {
      if (tracker === this.#firstTap) this.#reset();
      else this.#checkCancel();
    }
  }

  override dispose(): void {
    this.#reset();
    super.dispose();
  }

  #reset(): void {
    this.#stopTimer();
    const first = this.#firstTap;
    if (first !== null) {
      if (this.#trackers.size > 0) this.#checkCancel();
      this.#firstTap = null;
      first.entry.resolve("rejected");
      GestureBinding.instance.gestureArena.release(first.pointer);
    }
    for (const t of [...this.#trackers.values()]) {
      t.entry.resolve("rejected");
      this.#freeze(t);
    }
    this.#trackers.clear();
  }

  #registerFirstTap(tracker: TapTracker): void {
    this.#startTimer();
    GestureBinding.instance.gestureArena.hold(tracker.pointer);
    this.#freeze(tracker);
    this.#trackers.delete(tracker.pointer);
    this.#firstTap = tracker;
  }

  #registerSecondTap(tracker: TapTracker): void {
    const first = this.#firstTap!;
    first.entry.resolve("accepted");
    tracker.entry.resolve("accepted");
    this.#freeze(tracker);
    this.#trackers.delete(tracker.pointer);
    this.#firstTap = null;
    this.#stopTimer();
    GestureBinding.instance.gestureArena.release(first.pointer);
    for (const t of [...this.#trackers.values()]) this.#reject(t);
    if (this.onDoubleTap) this.invokeCallback("onDoubleTap", () => this.onDoubleTap!());
  }

  #freeze(tracker: TapTracker): void {
    if (!tracker.isTrackingPointer) return;
    tracker.isTrackingPointer = false;
    GestureBinding.instance.pointerRouter.removeRoute(tracker.pointer, tracker.route);
  }

  #startTimer(): void {
    this.#timer ??= SchedulerBinding.instance.scheduleDelayed(kDoubleTapTimeout, () => {
      this.#timer = null;
      this.#reset();
    });
  }

  #stopTimer(): void {
    this.#timer?.cancel();
    this.#timer = null;
  }

  #checkCancel(): void {
    if (this.onDoubleTapCancel) this.invokeCallback("onDoubleTapCancel", () => this.onDoubleTapCancel!());
  }
}
