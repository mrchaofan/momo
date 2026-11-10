import { reportError } from "../foundation/assert";
import type { Offset } from "../painting/geometry";
import { GestureArenaManager } from "./arena";
import { PointerEventConverter } from "./converter";
import {
  PointerAddedEvent, PointerCancelEvent, PointerDownEvent, PointerGestureEvent, PointerHoverEvent, PointerRemovedEvent,
  PointerScrollEvent, PointerSignalEvent, PointerUpEvent, type PointerEvent, type ScrollPhase,
} from "./events";
import { HitTestResult } from "./hit_test";
import { PointerRouter, PointerSignalResolver, type PointerRoute } from "./pointer_router";

/** 可接管指针按下序列的窗口(momo:ui View 的子集)。 */
export interface PointerCaptureTarget {
  readonly id: number;
  setCapture(): boolean;
}

/** 框架对原生指针事件的处置(momo:ui View.dispatchPlatformViewPointer 的 action)。 */
export const PlatformViewPointerAction = { event: 0, enter: 1, exit: 2 } as const;
export type PlatformViewPointerAction = (typeof PlatformViewPointerAction)[keyof typeof PlatformViewPointerAction];

/** 把处置交给宿主: viewId 为事件来源窗口(由 WidgetsBinding 设置为 view.dispatchPlatformViewPointer)。 */
export type PlatformViewPointerSink = (viewId: number, platformViewId: number, nativeEventId: number, action: PlatformViewPointerAction) => void;

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
  /** 进行中的捕获转交(transferPointerCapture): 目标视图、被认领的指针与接收者。 */
  #transfer: { viewId: number; pointer: number | null; route: PointerRoute } | null = null;
  /** 鼠标 hover 跟踪(由 WidgetsBinding 设置)。 */
  mouseTracker: MouseTrackerHook | null = null;
  /** 平台视图的原生事件出口(由 WidgetsBinding 设置;null = 宿主不支持, 转发被忽略)。 */
  platformViewPointerSink: PlatformViewPointerSink | null = null;
  /** 触控板手势锁定的命中结果(phase start → end): 滚动一份, 每种平台手势各一份(捏合与旋转可同时进行)。 */
  #signalLatches = new Map<string, HitTestResult>();
  /** 当前事件中: 原生事件 id → 已接收它的平台视图(去重;判断按下是否由 momo 内容消费)。 */
  #forwarded = new Map<number, Set<number>>();

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

  /**
   * 把进行中的鼠标按下序列转交给窗口 view(view.setCapture()), 并由 route 继续接收它——用于拖动中
   * 持有按下的窗口即将关闭(如把它的内容并入另一个窗口)时, 让同一次拖动不中断。
   * 宿主在 view 里补发的按下以及该指针之后的移动、抬起/取消只交给 route(不做命中测试、不进竞技场,
   * 因此目标窗口里的手势识别器不会把它当成新的拖动);抬起或取消后自动结束。
   * 返回取消函数;没有可转交的按下序列(按键已松开)时返回 null, route 不会被调用。
   */
  transferPointerCapture(view: PointerCaptureTarget, route: PointerRoute): (() => void) | null {
    if (!view.setCapture()) return null;
    const transfer = { viewId: view.id, pointer: null as number | null, route };
    this.#transfer = transfer;
    return () => {
      if (this.#transfer === transfer) this.#transfer = null;
    };
  }

  /** 属于捕获转交的事件: 交给接收者并返回 true。 */
  #routeTransfer(event: PointerEvent): boolean {
    const t = this.#transfer;
    if (t === null) return false;
    if (t.pointer === null) {
      if (!(event instanceof PointerDownEvent) || this.#viewId !== t.viewId) return false;
      t.pointer = event.pointer;  // 认领: 目标窗口里宿主补发的按下
    } else if (event.pointer !== t.pointer) {
      return false;
    }
    if (event instanceof PointerUpEvent || event instanceof PointerCancelEvent) this.#transfer = null;
    try {
      t.route(event);
    } catch (e) {
      reportError(e, "while routing a transferred pointer capture");
    }
    return true;
  }

  /**
   * 把原生事件交给平台视图 platformViewId(docs/05 §9 "输入";由 RenderPlatformView 在命中路径上调用)。
   * 宿主异步按 event.nativeEventId 取回原始事件投递给原生视图;合成事件(id 0)忽略。
   * 同一原生事件对同一平台视图只投递一次。
   */
  forwardToPlatformView(platformViewId: number, event: PointerEvent): void {
    const id = event.nativeEventId;
    if (id === 0) return;
    let receivers = this.#forwarded.get(id);
    if (receivers === undefined) this.#forwarded.set(id, receivers = new Set());
    if (receivers.has(platformViewId)) return;
    receivers.add(platformViewId);
    this.#sink(platformViewId, id, PlatformViewPointerAction.event);
  }

  /**
   * 指针归属移到 / 离开平台视图(由 RenderPlatformView 的 MouseTracker 进入/离开调用)。
   * nativeEventId: 触发它的原生事件(宿主取其位置与时间;0 = 无, 取当前指针位置)。
   * 与 forwardToPlatformView 走同一条有序通道: 原生视图按框架处理的顺序看到进入、事件、离开。
   */
  setPlatformViewHover(platformViewId: number, hovered: boolean, nativeEventId = 0): void {
    this.#sink(platformViewId, nativeEventId, hovered ? PlatformViewPointerAction.enter : PlatformViewPointerAction.exit);
  }

  #sink(platformViewId: number, nativeEventId: number, action: PlatformViewPointerAction): void {
    const sink = this.platformViewPointerSink;
    if (sink === null) return;
    try {
      sink(this.#viewId, platformViewId, nativeEventId, action);
    } catch (e) {
      reportError(e, "while dispatching a pointer event to a platform view");
    }
  }

  /** 处理单个指针事件。 */
  handlePointerEvent(event: PointerEvent): void {
    try {
      this.#handlePointerEvent(event);
    } finally {
      // 原生按下没有任何平台视图接收: 由 momo 内容消费, 宿主把键盘焦点交还 momo。
      if (event instanceof PointerDownEvent && event.nativeEventId !== 0 && !this.#forwarded.has(event.nativeEventId)) {
        this.#sink(0, event.nativeEventId, PlatformViewPointerAction.event);
      }
      this.#forwarded.clear();
    }
  }

  /**
   * 触控板手势内的信号沿用手势开始时的命中结果(手势中途指针下的内容变化也不换接收者);
   * 没有阶段的(普通滚轮、智能缩放…)与手势开始重新命中测试。
   */
  #signalHitTest(key: string, phase: ScrollPhase, position: Offset): HitTestResult {
    let result = phase === "update" || phase === "end" ? this.#signalLatches.get(key) ?? null : null;
    result ??= this.hitTest(position);
    if (phase === "start" || phase === "update") this.#signalLatches.set(key, result);
    else this.#signalLatches.delete(key);
    return result;
  }

  #handlePointerEvent(event: PointerEvent): void {
    if (this.#routeTransfer(event)) return;
    let result: HitTestResult | null = null;
    if (event instanceof PointerScrollEvent) {
      result = this.#signalHitTest("scroll", event.scrollPhase, event.position);
    } else if (event instanceof PointerGestureEvent) {
      // begin/end 是未细分手势的边界, 与捏合/旋转共用一份锁定。
      const key = event.gesture === "begin" || event.gesture === "end" ? "gesture" : event.gesture;
      result = this.#signalHitTest(key, event.phase, event.position);
    } else if (event instanceof PointerDownEvent || event instanceof PointerSignalEvent || event instanceof PointerHoverEvent || event instanceof PointerAddedEvent) {
      result = this.hitTest(event.position);
      if (event instanceof PointerDownEvent) this.#hitTests.set(event.pointer, result);
    } else if (event instanceof PointerUpEvent || event instanceof PointerCancelEvent) {
      result = this.#hitTests.get(event.pointer) ?? null;
      this.#hitTests.delete(event.pointer);
    } else if (event.down) {
      result = this.#hitTests.get(event.pointer) ?? null;
    }
    // 先更新悬停(进入/离开), 再分发(同 Flutter RendererBinding.dispatchEvent): 区域先"进入"再收到
    // 该位置的事件。平台视图依赖这个顺序——宿主按收到的顺序把进入/离开与事件交给原生视图。
    if (this.mouseTracker !== null) {
      const fresh = event instanceof PointerHoverEvent || event instanceof PointerAddedEvent ? result : null;
      this.mouseTracker.updateWithEvent(event, fresh);
    }
    if (result !== null || event instanceof PointerRemovedEvent || event instanceof PointerAddedEvent) {
      this.dispatchEvent(event, result);
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
    this.#transfer = null;
    this.#signalLatches.clear();
    this.#forwarded.clear();
    this.#hitTests.clear();
    this.pointerRouter.reset();
    this.gestureArena.reset();
    this.converter.reset();
  }
}
