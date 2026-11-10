import { reportError } from "../foundation/assert";
import type { Offset } from "../painting/geometry";
import { GestureArenaManager } from "./arena";
import { PointerEventConverter } from "./converter";
import {
  PointerAddedEvent, PointerCancelEvent, PointerDownEvent, PointerHoverEvent, PointerRemovedEvent, PointerSignalEvent,
  PointerUpEvent, type PointerEvent,
} from "./events";
import { HitTestResult } from "./hit_test";
import { PointerRouter, PointerSignalResolver } from "./pointer_router";

/** 鼠标跟踪钩子(由渲染层的 MouseTracker 实现)。 */
export interface MouseTrackerHook {
  updateWithEvent(event: PointerEvent, result: HitTestResult | null): void;
}

/**
 * 指针事件入口(对标 Flutter GestureBinding):解码指针包 → 命中测试 → 沿命中路径分发 → PointerRouter
 * → 竞技场 close/sweep, 滚轮经 PointerSignalResolver。
 */
export class GestureBinding {
  static #instance: GestureBinding | null = null;
  static get instance(): GestureBinding {
    if (GestureBinding.#instance === null) GestureBinding.#instance = new GestureBinding();
    return GestureBinding.#instance;
  }

  readonly pointerRouter = new PointerRouter();
  readonly gestureArena = new GestureArenaManager();
  readonly pointerSignalResolver = new PointerSignalResolver();
  readonly converter = new PointerEventConverter();
  #hitTests = new Map<number, HitTestResult>();

  /** 在视图 viewId(0 = 主窗口)的根做命中测试(由 WidgetsBinding 设置)。 */
  hitTestInView: ((result: HitTestResult, position: Offset, viewId: number) => void) | null = null;
  #viewId = 0;
  /** 鼠标 hover 跟踪(由 WidgetsBinding 设置)。 */
  mouseTracker: MouseTrackerHook | null = null;

  private constructor() {}

  /**
   * 解码并处理 momo:ui 指针包。viewId: 事件来自的视图(窗口), 坐标为该窗口的物理像素。
   * 多窗口共用一套指针路由与手势竞技场(同一时刻只有一个鼠标), 命中测试在来源窗口的渲染树上进行。
   */
  handlePointerPacket(data: Float64Array, count: number, devicePixelRatio: number, viewId = 0): void {
    this.#viewId = viewId;
    const events = this.converter.expand(data, count, devicePixelRatio);
    for (let i = 0; i < events.length; i++) this.handlePointerEvent(events[i]!);
  }

  /** 最近一次指针事件来自的视图(光标、hover 重算发往它)。 */
  get currentViewId(): number {
    return this.#viewId;
  }

  /** 在当前视图的 position 做命中测试。 */
  hitTest(position: Offset): HitTestResult {
    const result = new HitTestResult();
    this.hitTestInView?.(result, position, this.#viewId);
    return result;
  }

  /** 处理单个指针事件。 */
  handlePointerEvent(event: PointerEvent): void {
    let result: HitTestResult | null = null;
    if (event instanceof PointerDownEvent || event instanceof PointerSignalEvent || event instanceof PointerHoverEvent || event instanceof PointerAddedEvent) {
      result = this.hitTest(event.position);
      if (event instanceof PointerDownEvent) this.#hitTests.set(event.pointer, result);
    } else if (event instanceof PointerUpEvent || event instanceof PointerCancelEvent) {
      result = this.#hitTests.get(event.pointer) ?? null;
      this.#hitTests.delete(event.pointer);
    } else if (event.down) {
      result = this.#hitTests.get(event.pointer) ?? null;
    }
    if (result !== null || event instanceof PointerRemovedEvent || event instanceof PointerAddedEvent) {
      this.dispatchEvent(event, result);
    }
    if (this.mouseTracker !== null) {
      const fresh = event instanceof PointerHoverEvent || event instanceof PointerAddedEvent ? result : null;
      this.mouseTracker.updateWithEvent(event, fresh);
    }
  }

  /** 沿命中路径分发, 再交给路由与竞技场。 */
  dispatchEvent(event: PointerEvent, result: HitTestResult | null): void {
    if (result !== null) {
      for (const entry of result.path) {
        try {
          entry.target.handleEvent(event.transformed(entry.transform), entry);
        } catch (e) {
          reportError(e, "while dispatching a pointer event");
        }
      }
    }
    this.pointerRouter.route(event);
    if (event instanceof PointerDownEvent) this.gestureArena.close(event.pointer);
    else if (event instanceof PointerUpEvent) this.gestureArena.sweep(event.pointer);
    else if (event instanceof PointerCancelEvent) this.gestureArena.sweep(event.pointer);
    else if (event instanceof PointerSignalEvent) this.pointerSignalResolver.resolve(event);
  }

  /** 测试用:清空状态。 */
  debugReset(): void {
    this.#viewId = 0;
    this.#hitTests.clear();
    this.pointerRouter.reset();
    this.gestureArena.reset();
    this.converter.reset();
  }
}
