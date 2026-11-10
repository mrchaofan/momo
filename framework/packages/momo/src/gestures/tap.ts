import type { Offset } from "../painting/geometry";
import { kPressTimeout } from "./constants";
import {
  PointerCancelEvent, PointerUpEvent, kPrimaryButton, kSecondaryButton, type PointerDeviceKind, type PointerDownEvent, type PointerEvent,
} from "./events";
import { PrimaryPointerGestureRecognizer } from "./recognizer";

/** 按下详情。 */
export interface TapDownDetails {
  readonly globalPosition: Offset;
  readonly localPosition: Offset;
  readonly kind: PointerDeviceKind;
}
/** 抬起详情。 */
export interface TapUpDetails {
  readonly globalPosition: Offset;
  readonly localPosition: Offset;
  readonly kind: PointerDeviceKind;
}

/**
 * 单击识别(对标 Flutter TapGestureRecognizer):主键 onTapDown/onTapUp/onTap/onTapCancel,
 * 次键 onSecondaryTap*。按下 100ms(kPressTimeout)或赢得竞技场时发 tapDown。
 */
export class TapGestureRecognizer extends PrimaryPointerGestureRecognizer {
  onTapDown: ((d: TapDownDetails) => void) | null = null;
  onTapUp: ((d: TapUpDetails) => void) | null = null;
  onTap: (() => void) | null = null;
  onTapCancel: (() => void) | null = null;
  onSecondaryTap: (() => void) | null = null;
  onSecondaryTapDown: ((d: TapDownDetails) => void) | null = null;
  onSecondaryTapUp: ((d: TapUpDetails) => void) | null = null;
  onSecondaryTapCancel: (() => void) | null = null;

  #sentTapDown = false;
  #wonArenaForPrimaryPointer = false;
  #down: PointerDownEvent | null = null;
  #up: PointerUpEvent | null = null;

  constructor(o: { supportedDevices?: ReadonlySet<PointerDeviceKind> | null; debugOwner?: unknown } = {}) {
    super({ ...o, deadline: kPressTimeout });
  }

  override isPointerAllowed(event: PointerDownEvent): boolean {
    if (event.buttons === kPrimaryButton) {
      if (this.onTapDown === null && this.onTap === null && this.onTapUp === null && this.onTapCancel === null) return false;
    } else if (event.buttons === kSecondaryButton) {
      if (this.onSecondaryTap === null && this.onSecondaryTapDown === null && this.onSecondaryTapUp === null && this.onSecondaryTapCancel === null) return false;
    } else {
      return false;
    }
    return super.isPointerAllowed(event);
  }

  protected override addAllowedPointer(event: PointerDownEvent): void {
    if (this.state === "ready") {
      if (this.#down !== null && this.#up !== null) this.#reset();
      this.#down = event;
    }
    if (this.#down !== null) super.addAllowedPointer(event);
  }

  protected override handlePrimaryPointer(event: PointerEvent): void {
    if (event instanceof PointerUpEvent) {
      this.#up = event;
      this.#checkUp();
    } else if (event instanceof PointerCancelEvent) {
      this.resolve("rejected");
      if (this.#sentTapDown) this.#checkCancel();
      this.#reset();
    } else if (this.#down !== null && event.buttons !== this.#down.buttons) {
      this.resolve("rejected");
      if (this.primaryPointer !== null) this.stopTrackingPointer(this.primaryPointer);
    }
  }

  override resolve(disposition: "accepted" | "rejected"): void {
    if (this.#wonArenaForPrimaryPointer && disposition === "rejected") {
      if (this.#sentTapDown) this.#checkCancel();
      this.#reset();
    }
    super.resolve(disposition);
  }

  protected override didExceedDeadline(): void {
    this.#checkDown();
  }

  override acceptGesture(pointer: number): void {
    super.acceptGesture(pointer);
    if (pointer === this.primaryPointer) {
      this.#checkDown();
      this.#wonArenaForPrimaryPointer = true;
      this.#checkUp();
    }
  }

  override rejectGesture(pointer: number): void {
    super.rejectGesture(pointer);
    if (pointer === this.primaryPointer) {
      if (this.#sentTapDown) this.#checkCancel();
      this.#reset();
    }
  }

  #checkDown(): void {
    if (this.#sentTapDown || this.#down === null) return;
    const d = this.#down;
    const details: TapDownDetails = { globalPosition: d.position, localPosition: d.localPosition, kind: d.kind };
    if (d.buttons === kPrimaryButton) {
      if (this.onTapDown) this.invokeCallback("onTapDown", () => this.onTapDown!(details));
    } else if (d.buttons === kSecondaryButton) {
      if (this.onSecondaryTapDown) this.invokeCallback("onSecondaryTapDown", () => this.onSecondaryTapDown!(details));
    }
    this.#sentTapDown = true;
  }

  #checkUp(): void {
    if (!this.#wonArenaForPrimaryPointer || this.#up === null || this.#down === null) return;
    const d = this.#down;
    const u = this.#up;
    if (!this.#sentTapDown) this.#checkDown();
    const details: TapUpDetails = { globalPosition: u.position, localPosition: u.localPosition, kind: u.kind };
    if (d.buttons === kPrimaryButton) {
      if (this.onTapUp) this.invokeCallback("onTapUp", () => this.onTapUp!(details));
      if (this.onTap) this.invokeCallback("onTap", () => this.onTap!());
    } else if (d.buttons === kSecondaryButton) {
      if (this.onSecondaryTapUp) this.invokeCallback("onSecondaryTapUp", () => this.onSecondaryTapUp!(details));
      if (this.onSecondaryTap) this.invokeCallback("onSecondaryTap", () => this.onSecondaryTap!());
    }
    this.#reset();
  }

  #checkCancel(): void {
    const d = this.#down;
    if (d === null) return;
    if (d.buttons === kPrimaryButton) {
      if (this.onTapCancel) this.invokeCallback("onTapCancel", () => this.onTapCancel!());
    } else if (d.buttons === kSecondaryButton) {
      if (this.onSecondaryTapCancel) this.invokeCallback("onSecondaryTapCancel", () => this.onSecondaryTapCancel!());
    }
  }

  #reset(): void {
    this.#sentTapDown = false;
    this.#wonArenaForPrimaryPointer = false;
    this.#up = null;
    this.#down = null;
  }
}
