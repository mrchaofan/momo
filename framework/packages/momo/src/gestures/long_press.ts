import type { Offset } from "../painting/geometry";
import { kLongPressTimeout } from "./constants";
import { PointerCancelEvent, PointerMoveEvent, PointerUpEvent, kPrimaryButton, type PointerDeviceKind, type PointerDownEvent, type PointerEvent } from "./events";
import { PrimaryPointerGestureRecognizer } from "./recognizer";

/** 长按详情。 */
export interface LongPressDetails {
  readonly globalPosition: Offset;
  readonly localPosition: Offset;
}

/** 长按识别:按住 500ms 不移动即接受。 */
export class LongPressGestureRecognizer extends PrimaryPointerGestureRecognizer {
  onLongPressDown: ((d: LongPressDetails) => void) | null = null;
  onLongPressStart: ((d: LongPressDetails) => void) | null = null;
  onLongPress: (() => void) | null = null;
  onLongPressMoveUpdate: ((d: LongPressDetails) => void) | null = null;
  onLongPressEnd: ((d: LongPressDetails) => void) | null = null;
  onLongPressUp: (() => void) | null = null;
  onLongPressCancel: (() => void) | null = null;
  #longPressAccepted = false;
  #downPosition: LongPressDetails | null = null;

  constructor(o: { duration?: number; supportedDevices?: ReadonlySet<PointerDeviceKind> | null; debugOwner?: unknown } = {}) {
    super({ deadline: o.duration ?? kLongPressTimeout, postAcceptSlopTolerance: null, supportedDevices: o.supportedDevices ?? null, debugOwner: o.debugOwner });
  }

  override isPointerAllowed(event: PointerDownEvent): boolean {
    return event.buttons === kPrimaryButton && super.isPointerAllowed(event);
  }

  protected override addAllowedPointer(event: PointerDownEvent): void {
    super.addAllowedPointer(event);
    this.#downPosition = { globalPosition: event.position, localPosition: event.localPosition };
    if (this.onLongPressDown) this.invokeCallback("onLongPressDown", () => this.onLongPressDown!(this.#downPosition!));
  }

  protected override didExceedDeadline(): void {
    this.resolve("accepted");
    this.#longPressAccepted = true;
    const d = this.#downPosition!;
    if (this.onLongPressStart) this.invokeCallback("onLongPressStart", () => this.onLongPressStart!(d));
    if (this.onLongPress) this.invokeCallback("onLongPress", () => this.onLongPress!());
  }

  protected override handlePrimaryPointer(event: PointerEvent): void {
    if (event instanceof PointerUpEvent) {
      if (this.#longPressAccepted) {
        const d = { globalPosition: event.position, localPosition: event.localPosition };
        if (this.onLongPressEnd) this.invokeCallback("onLongPressEnd", () => this.onLongPressEnd!(d));
        if (this.onLongPressUp) this.invokeCallback("onLongPressUp", () => this.onLongPressUp!());
      } else {
        this.resolve("rejected");
        if (this.onLongPressCancel) this.invokeCallback("onLongPressCancel", () => this.onLongPressCancel!());
      }
      this.#longPressAccepted = false;
    } else if (event instanceof PointerCancelEvent) {
      this.resolve("rejected");
      if (this.onLongPressCancel) this.invokeCallback("onLongPressCancel", () => this.onLongPressCancel!());
      this.#longPressAccepted = false;
    } else if (event instanceof PointerMoveEvent && this.#longPressAccepted) {
      const d = { globalPosition: event.position, localPosition: event.localPosition };
      if (this.onLongPressMoveUpdate) this.invokeCallback("onLongPressMoveUpdate", () => this.onLongPressMoveUpdate!(d));
    }
  }

  override rejectGesture(pointer: number): void {
    if (pointer === this.primaryPointer && this.state === "possible" && !this.#longPressAccepted) {
      if (this.onLongPressCancel) this.invokeCallback("onLongPressCancel", () => this.onLongPressCancel!());
    }
    super.rejectGesture(pointer);
  }
}
