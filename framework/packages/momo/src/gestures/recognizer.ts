import { reportError } from "../foundation/assert";
import { Offset } from "../painting/geometry";
import type { Matrix4 } from "../painting/matrix4";
import type { DelayedTask } from "../scheduler/binding";
import { SchedulerBinding } from "../scheduler/binding";
import { GestureArenaEntry, type GestureArenaMember, type GestureDisposition } from "./arena";
import { GestureBinding } from "./binding";
import { kTouchSlop } from "./constants";
import {
  PointerCancelEvent, PointerMoveEvent, PointerUpEvent, type PointerDeviceKind, type PointerDownEvent, type PointerEvent,
} from "./events";

/** 全局 + 局部位置。 */
export interface OffsetPair {
  readonly global: Offset;
  readonly local: Offset;
}

/** 手势识别器基类。 */
export abstract class GestureRecognizer implements GestureArenaMember {
  /** 允许的设备(null = 全部)。 */
  supportedDevices: ReadonlySet<PointerDeviceKind> | null;
  readonly debugOwner: unknown;
  #pointerKinds = new Map<number, PointerDeviceKind>();

  constructor(o: { supportedDevices?: ReadonlySet<PointerDeviceKind> | null; debugOwner?: unknown } = {}) {
    this.supportedDevices = o.supportedDevices ?? null;
    this.debugOwner = o.debugOwner;
  }

  /** 由 GestureDetector 在 PointerDown 时调用。 */
  addPointer(event: PointerDownEvent): void {
    this.#pointerKinds.set(event.pointer, event.kind);
    if (this.isPointerAllowed(event)) this.addAllowedPointer(event);
    else this.handleNonAllowedPointer(event);
  }

  /** 指针设备类型。 */
  getKindForPointer(pointer: number): PointerDeviceKind {
    return this.#pointerKinds.get(pointer) ?? "touch";
  }

  protected abstract addAllowedPointer(event: PointerDownEvent): void;
  protected handleNonAllowedPointer(_event: PointerDownEvent): void {}

  /** 是否接收该指针。 */
  isPointerAllowed(event: PointerDownEvent): boolean {
    return this.supportedDevices === null || this.supportedDevices.has(event.kind);
  }

  abstract acceptGesture(pointer: number): void;
  abstract rejectGesture(pointer: number): void;

  /** 释放。 */
  dispose(): void {}

  /** 调用用户回调(捕获异常)。 */
  protected invokeCallback<T>(name: string, callback: () => T): T | undefined {
    try {
      return callback();
    } catch (e) {
      reportError(e, `while handling a gesture (${name})`);
      return undefined;
    }
  }
}

/** 跟踪一组指针、整体参与竞技场的识别器。 */
export abstract class OneSequenceGestureRecognizer extends GestureRecognizer {
  #entries = new Map<number, GestureArenaEntry>();
  #trackedPointers = new Set<number>();
  #route = (event: PointerEvent): void => this.handleEvent(event);

  protected override addAllowedPointer(event: PointerDownEvent): void {
    this.startTrackingPointer(event.pointer, event.transform);
  }

  /** 处理被跟踪指针的事件。 */
  abstract handleEvent(event: PointerEvent): void;

  override acceptGesture(_pointer: number): void {}
  override rejectGesture(_pointer: number): void {}

  /** 最后一个被跟踪的指针停止跟踪。 */
  protected abstract didStopTrackingLastPointer(pointer: number): void;

  /** 对所有指针宣告裁决。 */
  resolve(disposition: GestureDisposition): void {
    const entries = [...this.#entries.values()];
    this.#entries.clear();
    for (const e of entries) e.resolve(disposition);
  }

  /** 对单个指针宣告裁决。 */
  resolvePointer(pointer: number, disposition: GestureDisposition): void {
    const e = this.#entries.get(pointer);
    if (e !== undefined) {
      this.#entries.delete(pointer);
      e.resolve(disposition);
    }
  }

  override dispose(): void {
    this.resolve("rejected");
    for (const p of [...this.#trackedPointers]) GestureBinding.instance.pointerRouter.removeRoute(p, this.#route);
    this.#trackedPointers.clear();
    super.dispose();
  }

  protected startTrackingPointer(pointer: number, transform: Matrix4 | null = null): void {
    const g = GestureBinding.instance;
    g.pointerRouter.addRoute(pointer, this.#route, transform);
    this.#trackedPointers.add(pointer);
    this.#entries.set(pointer, g.gestureArena.add(pointer, this));
  }

  protected stopTrackingPointer(pointer: number): void {
    if (!this.#trackedPointers.has(pointer)) return;
    GestureBinding.instance.pointerRouter.removeRoute(pointer, this.#route);
    this.#trackedPointers.delete(pointer);
    if (this.#trackedPointers.size === 0) this.didStopTrackingLastPointer(pointer);
  }

  protected stopTrackingIfPointerNoLongerDown(event: PointerEvent): void {
    if (event instanceof PointerUpEvent || event instanceof PointerCancelEvent) this.stopTrackingPointer(event.pointer);
  }

  get trackedPointerCount(): number {
    return this.#trackedPointers.size;
  }
}

/** 识别器状态。 */
export type GestureRecognizerState = "ready" | "possible" | "defunct";

/**
 * 跟踪单一主指针的识别器(tap/long press 的基类):移动超出 slop 即放弃;可设截止时间。
 */
export abstract class PrimaryPointerGestureRecognizer extends OneSequenceGestureRecognizer {
  /** 截止时间(毫秒);到时调用 didExceedDeadline。 */
  deadline: number | null;
  preAcceptSlopTolerance: number | null;
  postAcceptSlopTolerance: number | null;
  state: GestureRecognizerState = "ready";
  primaryPointer: number | null = null;
  initialPosition: OffsetPair | null = null;
  #gestureAccepted = false;
  #timer: DelayedTask | null = null;

  constructor(o: {
    deadline?: number | null;
    preAcceptSlopTolerance?: number | null;
    postAcceptSlopTolerance?: number | null;
    supportedDevices?: ReadonlySet<PointerDeviceKind> | null;
    debugOwner?: unknown;
  } = {}) {
    super(o);
    this.deadline = o.deadline ?? null;
    this.preAcceptSlopTolerance = o.preAcceptSlopTolerance === undefined ? kTouchSlop : o.preAcceptSlopTolerance;
    this.postAcceptSlopTolerance = o.postAcceptSlopTolerance === undefined ? kTouchSlop : o.postAcceptSlopTolerance;
  }

  protected override addAllowedPointer(event: PointerDownEvent): void {
    super.addAllowedPointer(event);
    if (this.state === "ready") {
      this.state = "possible";
      this.primaryPointer = event.pointer;
      this.initialPosition = { global: event.position, local: event.localPosition };
      if (this.deadline !== null) {
        this.#timer = SchedulerBinding.instance.scheduleDelayed(this.deadline, () => this.didExceedDeadlineWithEvent(event));
      }
    }
  }

  override handleEvent(event: PointerEvent): void {
    if (this.state === "possible" && event.pointer === this.primaryPointer) {
      const dist = this.#globalDistance(event);
      const pre = !this.#gestureAccepted && this.preAcceptSlopTolerance !== null && dist > this.preAcceptSlopTolerance;
      const post = this.#gestureAccepted && this.postAcceptSlopTolerance !== null && dist > this.postAcceptSlopTolerance;
      if (event instanceof PointerMoveEvent && (pre || post)) {
        this.resolve("rejected");
        this.stopTrackingPointer(this.primaryPointer);
      } else {
        this.handlePrimaryPointer(event);
      }
    }
    this.stopTrackingIfPointerNoLongerDown(event);
  }

  /** 主指针事件(未超出 slop)。 */
  protected abstract handlePrimaryPointer(event: PointerEvent): void;

  /** 截止时间到。 */
  protected didExceedDeadline(): void {}
  protected didExceedDeadlineWithEvent(_event: PointerDownEvent): void {
    this.didExceedDeadline();
  }

  override acceptGesture(pointer: number): void {
    if (pointer === this.primaryPointer) {
      this.#stopTimer();
      this.#gestureAccepted = true;
    }
  }

  override rejectGesture(pointer: number): void {
    if (pointer === this.primaryPointer && this.state === "possible") {
      this.#stopTimer();
      this.state = "defunct";
    }
  }

  protected override didStopTrackingLastPointer(_pointer: number): void {
    this.#stopTimer();
    this.state = "ready";
    this.#gestureAccepted = false;
  }

  override dispose(): void {
    this.#stopTimer();
    super.dispose();
  }

  #stopTimer(): void {
    this.#timer?.cancel();
    this.#timer = null;
  }

  #globalDistance(event: PointerEvent): number {
    const init = this.initialPosition;
    return init === null ? 0 : event.position.minus(init.global).distance;
  }
}
