import { Offset } from "../painting/geometry";
import { kMinFlingVelocity, kPanSlop, kPrecisePointerHitSlop, kPrecisePointerPanSlop, kTouchSlop } from "./constants";
import {
  PointerCancelEvent, PointerDownEvent, PointerMoveEvent, PointerUpEvent, isPrecisePointer, kPrimaryButton,
  type PointerDeviceKind, type PointerEvent,
} from "./events";
import { OneSequenceGestureRecognizer } from "./recognizer";
import { VelocityTracker } from "./velocity_tracker";

/** 拖动按下。 */
export interface DragDownDetails {
  readonly globalPosition: Offset;
  readonly localPosition: Offset;
  /** 屏幕位置(逻辑点, 主屏左上原点;见 PointerEvent.screenPosition)。跨窗口拖拽用它计算。 */
  readonly screenPosition: Offset;
}
/** 拖动开始。 */
export interface DragStartDetails {
  readonly globalPosition: Offset;
  readonly localPosition: Offset;
  readonly screenPosition: Offset;
  readonly sourceTimeStamp: number;
  readonly kind: PointerDeviceKind;
}
/** 拖动更新。delta 为本次位移(方向受限的识别器只含对应分量);primaryDelta 为单轴识别器的标量位移。 */
export interface DragUpdateDetails {
  readonly delta: Offset;
  readonly primaryDelta: number | null;
  readonly globalPosition: Offset;
  readonly localPosition: Offset;
  readonly screenPosition: Offset;
  readonly sourceTimeStamp: number;
}
/** 拖动结束(velocity 逻辑像素/秒)。 */
export interface DragEndDetails {
  readonly velocity: Offset;
  readonly primaryVelocity: number | null;
}

/** 开始位置取自:按下点 / 超出 slop 的点。 */
export type DragStartBehavior = "down" | "start";

type DragState = "ready" | "possible" | "accepted";

/** 拖动识别器基类(对标 Flutter DragGestureRecognizer)。 */
export abstract class DragGestureRecognizer extends OneSequenceGestureRecognizer {
  onDown: ((d: DragDownDetails) => void) | null = null;
  onStart: ((d: DragStartDetails) => void) | null = null;
  onUpdate: ((d: DragUpdateDetails) => void) | null = null;
  onEnd: ((d: DragEndDetails) => void) | null = null;
  onCancel: (() => void) | null = null;
  dragStartBehavior: DragStartBehavior = "start";
  minFlingVelocity = kMinFlingVelocity;

  #state: DragState = "ready";
  #initialGlobal = Offset.zero;
  #initialLocal = Offset.zero;
  #initialScreen = Offset.zero;
  #lastScreen = Offset.zero;
  #pendingGlobal = Offset.zero;
  #pendingLocal = Offset.zero;
  #lastTimeStamp = 0;
  #initialButtons = 0;
  #kind: PointerDeviceKind = "touch";
  #globalDistanceMoved = 0;
  #velocityTrackers = new Map<number, VelocityTracker>();

  /** 位移在本识别器方向上的投影。 */
  protected abstract getDeltaForDetails(delta: Offset): Offset;
  protected abstract getPrimaryValueFromOffset(value: Offset): number | null;
  protected abstract hasSufficientGlobalDistanceToAccept(kind: PointerDeviceKind): boolean;
  protected abstract isFlingGesture(velocity: Offset): boolean;

  /** 已移动的全局距离(单轴为带符号投影, pan 为欧氏距离)。 */
  protected get globalDistanceMoved(): number {
    return this.#globalDistanceMoved;
  }
  protected get pendingGlobalOffset(): Offset {
    return this.#pendingGlobal;
  }

  override isPointerAllowed(event: PointerDownEvent): boolean {
    if (this.#initialButtons === 0) {
      if (event.buttons !== kPrimaryButton) return false;
    } else if (event.buttons !== this.#initialButtons) {
      return false;
    }
    return super.isPointerAllowed(event);
  }

  protected override addAllowedPointer(event: PointerDownEvent): void {
    super.addAllowedPointer(event);
    const tracker = new VelocityTracker();
    tracker.addPosition(event.timeStamp, event.localPosition);
    this.#velocityTrackers.set(event.pointer, tracker);
    if (this.#state === "ready") {
      this.#state = "possible";
      this.#initialGlobal = event.position;
      this.#initialLocal = event.localPosition;
      this.#initialScreen = event.screenPosition;
      this.#lastScreen = event.screenPosition;
      this.#initialButtons = event.buttons;
      this.#kind = event.kind;
      this.#pendingGlobal = Offset.zero;
      this.#pendingLocal = Offset.zero;
      this.#globalDistanceMoved = 0;
      this.#lastTimeStamp = event.timeStamp;
      if (this.onDown) {
        const d: DragDownDetails = { globalPosition: this.#initialGlobal, localPosition: this.#initialLocal, screenPosition: this.#initialScreen };
        this.invokeCallback("onDown", () => this.onDown!(d));
      }
    } else if (this.#state === "accepted") {
      this.resolve("accepted");
    }
  }

  override handleEvent(event: PointerEvent): void {
    if (event instanceof PointerMoveEvent || event instanceof PointerDownEvent) {
      this.#velocityTrackers.get(event.pointer)?.addPosition(event.timeStamp, event.localPosition);
    }
    if (event instanceof PointerMoveEvent) {
      if (event.buttons !== this.#initialButtons) {
        this.#giveUpPointer(event.pointer);
        return;
      }
      this.#lastScreen = event.screenPosition;
      if (this.#state === "accepted") {
        this.#checkUpdate(event.delta, event.localDelta, event.position, event.localPosition, event.timeStamp);
      } else {
        this.#pendingGlobal = this.#pendingGlobal.plus(event.delta);
        this.#pendingLocal = this.#pendingLocal.plus(event.localDelta);
        this.#lastTimeStamp = event.timeStamp;
        const movedLocally = this.getDeltaForDetails(event.localDelta);
        const primary = this.getPrimaryValueFromOffset(movedLocally);
        this.#globalDistanceMoved += primary !== null ? primary : movedLocally.distance;
        if (this.hasSufficientGlobalDistanceToAccept(event.kind)) this.resolve("accepted");
      }
    }
    if (event instanceof PointerUpEvent || event instanceof PointerCancelEvent) {
      this.#giveUpPointer(event.pointer);
    }
  }

  override acceptGesture(_pointer: number): void {
    if (this.#state === "accepted") return;
    this.#state = "accepted";
    const delta = this.#pendingGlobal;
    const localDelta = this.#pendingLocal;
    const ts = this.#lastTimeStamp;
    let startGlobal = this.#initialGlobal;
    let startLocal = this.#initialLocal;
    let startScreen = this.#initialScreen;
    if (this.dragStartBehavior === "start") {
      startGlobal = this.#initialGlobal.plus(delta);
      startLocal = this.#initialLocal.plus(localDelta);
      startScreen = this.#lastScreen;
    }
    this.#pendingGlobal = Offset.zero;
    this.#pendingLocal = Offset.zero;
    if (this.onStart) {
      const d: DragStartDetails = { globalPosition: startGlobal, localPosition: startLocal, screenPosition: startScreen, sourceTimeStamp: ts, kind: this.#kind };
      this.invokeCallback("onStart", () => this.onStart!(d));
    }
    if (this.dragStartBehavior === "down" && (delta.dx !== 0 || delta.dy !== 0)) {
      this.#checkUpdate(delta, localDelta, this.#initialGlobal.plus(delta), this.#initialLocal.plus(localDelta), ts);
    }
  }

  override rejectGesture(pointer: number): void {
    this.#giveUpPointer(pointer);
  }

  protected override didStopTrackingLastPointer(pointer: number): void {
    if (this.#state === "possible") {
      this.resolve("rejected");
      if (this.onCancel) this.invokeCallback("onCancel", () => this.onCancel!());
    } else if (this.#state === "accepted") {
      this.#checkEnd(pointer);
    }
    this.#velocityTrackers.clear();
    this.#initialButtons = 0;
    this.#state = "ready";
  }

  #giveUpPointer(pointer: number): void {
    this.stopTrackingPointer(pointer);
    this.resolvePointer(pointer, "rejected");
  }

  #checkUpdate(delta: Offset, localDelta: Offset, global: Offset, local: Offset, ts: number): void {
    if (!this.onUpdate) return;
    const d = this.getDeltaForDetails(localDelta);
    const details: DragUpdateDetails = {
      delta: d,
      primaryDelta: this.getPrimaryValueFromOffset(d),
      globalPosition: global,
      localPosition: local,
      screenPosition: this.#lastScreen,
      sourceTimeStamp: ts,
    };
    void delta;
    this.invokeCallback("onUpdate", () => this.onUpdate!(details));
  }

  #checkEnd(pointer: number): void {
    if (!this.onEnd) return;
    const tracker = this.#velocityTrackers.get(pointer);
    let velocity = tracker ? tracker.getVelocity() : Offset.zero;
    if (!this.isFlingGesture(velocity)) velocity = Offset.zero;
    velocity = this.getDeltaForDetails(velocity);
    const details: DragEndDetails = { velocity, primaryVelocity: this.getPrimaryValueFromOffset(velocity) };
    this.invokeCallback("onEnd", () => this.onEnd!(details));
  }

  override dispose(): void {
    this.#velocityTrackers.clear();
    super.dispose();
  }
}

function hitSlop(kind: PointerDeviceKind): number {
  return isPrecisePointer(kind) ? kPrecisePointerHitSlop : kTouchSlop;
}
function panSlop(kind: PointerDeviceKind): number {
  return isPrecisePointer(kind) ? kPrecisePointerPanSlop : kPanSlop;
}

/** 纵向拖动。 */
export class VerticalDragGestureRecognizer extends DragGestureRecognizer {
  protected getDeltaForDetails(delta: Offset): Offset {
    return new Offset(0, delta.dy);
  }
  protected getPrimaryValueFromOffset(value: Offset): number | null {
    return value.dy;
  }
  protected hasSufficientGlobalDistanceToAccept(kind: PointerDeviceKind): boolean {
    return Math.abs(this.globalDistanceMoved) > hitSlop(kind);
  }
  protected isFlingGesture(velocity: Offset): boolean {
    return Math.abs(velocity.dy) > this.minFlingVelocity;
  }
}

/** 横向拖动。 */
export class HorizontalDragGestureRecognizer extends DragGestureRecognizer {
  protected getDeltaForDetails(delta: Offset): Offset {
    return new Offset(delta.dx, 0);
  }
  protected getPrimaryValueFromOffset(value: Offset): number | null {
    return value.dx;
  }
  protected hasSufficientGlobalDistanceToAccept(kind: PointerDeviceKind): boolean {
    return Math.abs(this.globalDistanceMoved) > hitSlop(kind);
  }
  protected isFlingGesture(velocity: Offset): boolean {
    return Math.abs(velocity.dx) > this.minFlingVelocity;
  }
}

/** 任意方向拖动。 */
export class PanGestureRecognizer extends DragGestureRecognizer {
  protected getDeltaForDetails(delta: Offset): Offset {
    return delta;
  }
  protected getPrimaryValueFromOffset(_value: Offset): number | null {
    return null;
  }
  protected hasSufficientGlobalDistanceToAccept(kind: PointerDeviceKind): boolean {
    return this.pendingGlobalOffset.distance > panSlop(kind);
  }
  protected isFlingGesture(velocity: Offset): boolean {
    return velocity.distanceSquared > this.minFlingVelocity * this.minFlingVelocity;
  }
}
