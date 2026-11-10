import type { SystemCursor } from "momo:ui";
import { PointerEnterEvent, PointerExitEvent, PointerRemovedEvent, type PointerEvent } from "../gestures/events";
import type { HitTestResult } from "../gestures/hit_test";
import type { Matrix4 } from "../painting/matrix4";
import type { Offset } from "../painting/geometry";

/** 鼠标光标:系统光标名, 或 "defer"(交给下层区域决定)。 */
export type MouseCursor = SystemCursor | "defer";

/** MouseTrackerAnnotation 标记符。 */
export const kMouseTrackerAnnotation: unique symbol = Symbol("MouseTrackerAnnotation");

/** 参与 hover 跟踪的对象(如 RenderMouseRegion)。 */
export interface MouseTrackerAnnotation {
  readonly [kMouseTrackerAnnotation]: true;
  readonly onEnter: ((event: PointerEnterEvent) => void) | null;
  readonly onExit: ((event: PointerExitEvent) => void) | null;
  readonly cursor: MouseCursor;
  readonly validForMouseTracker: boolean;
}

function isAnnotation(o: unknown): o is MouseTrackerAnnotation {
  return typeof o === "object" && o !== null && (o as { [kMouseTrackerAnnotation]?: unknown })[kMouseTrackerAnnotation] === true;
}

interface DeviceState {
  annotations: Map<MouseTrackerAnnotation, Matrix4 | null>;
  lastEvent: PointerEvent;
}

/**
 * 跟踪鼠标所在的 MouseRegion 集合, 合成 enter/exit 事件并更新光标(window.setCursor)。
 */
export class MouseTracker {
  #devices = new Map<number, DeviceState>();
  #hitTest: (position: Offset) => HitTestResult;
  #setCursor: (cursor: SystemCursor) => void;
  #cursor: SystemCursor = "basic";

  constructor(hitTest: (position: Offset) => HitTestResult, setCursor: (cursor: SystemCursor) => void) {
    this.#hitTest = hitTest;
    this.#setCursor = setCursor;
  }

  /** 是否有鼠标设备接入。 */
  get mouseIsConnected(): boolean {
    return this.#devices.size > 0;
  }

  /** 当前光标。 */
  get currentCursor(): SystemCursor {
    return this.#cursor;
  }

  /** 处理一个鼠标事件(result 为该位置的命中结果, 无则内部计算)。 */
  updateWithEvent(event: PointerEvent, result: HitTestResult | null): void {
    if (event.kind !== "mouse" && event.kind !== "trackpad" && event.kind !== "stylus") return;
    if (event instanceof PointerRemovedEvent) {
      const state = this.#devices.get(event.device);
      if (state) {
        this.#dispatch(state.annotations, new Map(), event);
        this.#devices.delete(event.device);
      }
      this.#updateCursor();
      return;
    }
    const hit = result ?? this.#hitTest(event.position);
    const next = new Map<MouseTrackerAnnotation, Matrix4 | null>();
    for (const entry of hit.path) {
      if (isAnnotation(entry.target) && entry.target.validForMouseTracker && !next.has(entry.target)) next.set(entry.target, entry.transform);
    }
    const state = this.#devices.get(event.device);
    const prev = state?.annotations ?? new Map<MouseTrackerAnnotation, Matrix4 | null>();
    this.#devices.set(event.device, { annotations: next, lastEvent: event });
    this.#dispatch(prev, next, event);
    this.#updateCursor();
  }

  /** 布局变化后(帧尾)以最后位置重新计算 hover 状态。 */
  updateAllDevices(): void {
    for (const state of [...this.#devices.values()]) {
      const e = state.lastEvent;
      this.updateWithEvent(e, null);
    }
  }

  #dispatch(prev: Map<MouseTrackerAnnotation, Matrix4 | null>, next: Map<MouseTrackerAnnotation, Matrix4 | null>, event: PointerEvent): void {
    // exit:从内到外
    for (const [a, t] of prev) {
      if (next.has(a)) continue;
      // 已从树上移除的区域不再接收 exit(与 Flutter 一致)。
      if (a.validForMouseTracker) {
        a.onExit?.(new PointerExitEvent({ timeStamp: event.timeStamp, pointer: event.pointer, kind: event.kind, device: event.device, position: event.position, screenPosition: event.screenPosition, nativeEventId: event.nativeEventId }).transformed(t));
      }
    }
    // enter:从外到内
    const entering: [MouseTrackerAnnotation, Matrix4 | null][] = [];
    for (const [a, t] of next) if (!prev.has(a)) entering.push([a, t]);
    for (let i = entering.length - 1; i >= 0; i--) {
      const [a, t] = entering[i]!;
      a.onEnter?.(new PointerEnterEvent({ timeStamp: event.timeStamp, pointer: event.pointer, kind: event.kind, device: event.device, position: event.position, screenPosition: event.screenPosition, nativeEventId: event.nativeEventId }).transformed(t));
    }
  }

  #updateCursor(): void {
    let cursor: SystemCursor = "basic";
    // 取任一设备(通常只有一个鼠标)最内层非 defer 的光标
    for (const state of this.#devices.values()) {
      for (const a of state.annotations.keys()) {
        if (a.cursor !== "defer") {
          cursor = a.cursor;
          break;
        }
      }
    }
    if (cursor !== this.#cursor) {
      this.#cursor = cursor;
      this.#setCursor(cursor);
    }
  }

  /** 测试用:清空。 */
  reset(): void {
    this.#devices.clear();
    this.#cursor = "basic";
  }
}
