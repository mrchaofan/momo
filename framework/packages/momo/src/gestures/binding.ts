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

/**
 * 一次显式指针捕获(GestureBinding.capturePointer): 进行中的按下序列从此只交给捕获者, 直到抬起或取消。
 * 捕获者不属于任何 view 或窗口——触发拖动的 view 卸载、它所在的窗口关闭, 序列照常交给捕获者。
 */
export class PointerCapture {
  #active = true;
  constructor(readonly pointer: number, readonly sequenceId: number, readonly binding: GestureBinding) {}

  /** 捕获仍有效(序列未结束、未释放)。 */
  get active(): boolean {
    return this.#active;
  }

  /**
   * 原生事件改由窗口 viewId 接收(宿主的原生捕获;拖出标签成窗、并入另一个窗口)。逻辑接收方不变,
   * 同一个指针继续——之后的事件来自 viewId(坐标是它的, screenPosition 不变)。序列已结束时什么也不做。
   */
  moveToView(viewId: number): void {
    if (this.#active) this.binding.pointerCaptureSink?.(viewId, this.sequenceId);
  }

  /** 提前释放: 之后到抬起为止这个指针的事件被丢弃(原接收方已失去它, 不会恢复)。 */
  release(): void {
    this.binding._releaseCapture(this);
  }

  /** @internal */
  _end(): void {
    this.#active = false;
  }
}

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

  /** 在视图 viewId 的根做命中测试(由 ViewsBinding 设置)。 */
  hitTestInView: ((result: HitTestResult, position: Offset, viewId: number) => void) | null = null;
  /** 宿主的原生捕获: 进行中的序列 sequenceId 改由窗口 viewId 接收(由 ViewsBinding 设置为 Widget.capturePointer)。 */
  pointerCaptureSink: ((viewId: number, sequenceId: number) => void) | null = null;
  /** 显式捕获: 指针 → 捕获者。 */
  readonly #captures = new Map<number, { capture: PointerCapture; route: PointerRoute }>();
  /** 正在分发的指针事件层数;分发中发起的捕获, 原接收方的取消推迟到分发结束(路由按快照分发)。 */
  #dispatching = 0;
  #pendingCancels: [PointerCancelEvent, HitTestResult | null][] = [];
  /** 鼠标 hover 跟踪(由 ViewsBinding 设置)。 */
  mouseTracker: MouseTrackerHook | null = null;
  /** 触控板手势锁定的命中结果(phase start → end): 滚动一份, 每种平台手势各一份(捏合与旋转可同时进行)。 */
  #signalLatches = new Map<string, HitTestResult>();

  private constructor() {}

  /**
   * 解码并处理 momo:ui 指针包。viewId: 事件来自的视图(窗口), 坐标为该窗口的物理像素;它记在每个事件上
   * (PointerEvent.viewId)。多窗口共用一套指针路由与手势竞技场(同一时刻只有一个鼠标), 命中测试在事件
   * 所属窗口的渲染树上进行。
   */
  handlePointerPacket(data: Float64Array, count: number, devicePixelRatio: number, viewId = 0): void {
    const events = this.converter.expand(data, count, devicePixelRatio, viewId);
    for (let i = 0; i < events.length; i++) this.handlePointerEvent(events[i]!);
  }

  /** 在视图 viewId 的 position 做命中测试。 */
  hitTest(position: Offset, viewId = 0): HitTestResult {
    const result = new HitTestResult();
    this.hitTestInView?.(result, position, viewId);
    return result;
  }

  /**
   * 显式捕获进行中的按下序列(同 Win32 SetCapture / Chromium View::SetCapture): 从此这个指针的全部事件
   * (不论来自哪个窗口)只交给 route——不做命中测试、不进竞技场、不更新悬停;抬起或取消后自动结束。
   * 原接收方(按下时的命中路径、已在竞技场里的手势)失去这个指针, 收到取消(分发中发起时推迟到当前事件分发完)。
   * 典型用法: 拖动开始时捕获(拖动会话成为接收方), 换窗口时 capture.moveToView(viewId)。
   * device: 缺省为任一按下中的设备。没有进行中的按下序列时返回 null。
   */
  capturePointer(route: PointerRoute, o: { device?: number } = {}): PointerCapture | null {
    const active = this.converter.activePointer(o.device);
    if (active === null) return null;
    const pointer = active.pointer;
    this.#captures.get(pointer)?.capture._end();  // 重复捕获: 新的捕获者接替
    const capture = new PointerCapture(pointer, active.sequenceId, this);
    this.#captures.set(pointer, { capture, route });
    const old = this.#hitTests.get(pointer) ?? null;
    this.#hitTests.delete(pointer);
    const cancel = new PointerCancelEvent({
      pointer, device: active.device, viewId: active.viewId, kind: "mouse",
      position: active.position, screenPosition: active.screenPosition,
    });
    if (this.#dispatching > 0) this.#pendingCancels.push([cancel, old]);
    else this.#cancelPrevious(cancel, old);
    return capture;
  }

  /** @internal PointerCapture.release。 */
  _releaseCapture(capture: PointerCapture): void {
    const entry = this.#captures.get(capture.pointer);
    if (entry?.capture !== capture) return;
    capture._end();
    this.#captures.set(capture.pointer, { capture, route: () => {} });  // 到抬起为止丢弃
  }

  /** 原接收方失去被捕获的指针: 命中路径与路由收到取消, 竞技场清场。 */
  #cancelPrevious(cancel: PointerCancelEvent, old: HitTestResult | null): void {
    this.dispatchEvent(cancel, old);
  }

  /** 属于显式捕获的事件: 交给捕获者并返回 true。 */
  #routeCaptured(event: PointerEvent): boolean {
    const entry = this.#captures.get(event.pointer);
    if (entry === undefined || event instanceof PointerAddedEvent || event instanceof PointerHoverEvent ||
        event instanceof PointerRemovedEvent) {
      return false;
    }
    const end = event instanceof PointerUpEvent || event instanceof PointerCancelEvent;
    if (end) {
      this.#captures.delete(event.pointer);
      entry.capture._end();
    }
    try {
      entry.route(event);
    } catch (e) {
      reportError(e, "while routing a captured pointer");
    }
    // 序列结束: 悬停按松开处重新计算(按下期间冻结, 见 MouseTracker)。
    if (end) this.mouseTracker?.updateWithEvent(event, null);
    return true;
  }

  /** 处理单个指针事件。 */
  handlePointerEvent(event: PointerEvent): void {
    this.#dispatching++;
    try {
      this.#handlePointerEvent(event);
    } finally {
      this.#dispatching--;
      if (this.#dispatching === 0) {
        const pending = this.#pendingCancels;
        this.#pendingCancels = [];
        for (const [cancel, old] of pending) this.#cancelPrevious(cancel, old);
      }
    }
  }

  /**
   * 触控板手势内的信号沿用手势开始时的命中结果(手势中途指针下的内容变化也不换接收者);
   * 没有阶段的(普通滚轮、智能缩放…)与手势开始重新命中测试。
   */
  #signalHitTest(key: string, phase: ScrollPhase, event: PointerEvent): HitTestResult {
    let result = phase === "update" || phase === "end" ? this.#signalLatches.get(key) ?? null : null;
    result ??= this.hitTest(event.position, event.viewId);
    if (phase === "start" || phase === "update") this.#signalLatches.set(key, result);
    else this.#signalLatches.delete(key);
    return result;
  }

  #handlePointerEvent(event: PointerEvent): void {
    if (this.#routeCaptured(event)) return;
    let result: HitTestResult | null = null;
    if (event instanceof PointerScrollEvent) {
      result = this.#signalHitTest("scroll", event.scrollPhase, event);
    } else if (event instanceof PointerGestureEvent) {
      // begin/end 是未细分手势的边界, 与捏合/旋转共用一份锁定。
      const key = event.gesture === "begin" || event.gesture === "end" ? "gesture" : event.gesture;
      result = this.#signalHitTest(key, event.phase, event);
    } else if (event instanceof PointerDownEvent || event instanceof PointerSignalEvent || event instanceof PointerHoverEvent || event instanceof PointerAddedEvent) {
      result = this.hitTest(event.position, event.viewId);
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
    for (const { capture } of this.#captures.values()) capture._end();
    this.#captures.clear();
    this.#pendingCancels = [];
    this.#dispatching = 0;
    this.#signalLatches.clear();
    this.#hitTests.clear();
    this.pointerRouter.reset();
    this.gestureArena.reset();
    this.converter.reset();
  }
}
