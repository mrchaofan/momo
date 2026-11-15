import { platformDispatcher } from "momo:ui";
import { reportError } from "../foundation/assert";

/** 帧阶段。 */
export enum SchedulerPhase {
  /** 无帧在处理。 */
  idle = 0,
  /** onBeginFrame:推进动画(transient 回调)。 */
  transientCallbacks = 1,
  /** onDrawFrame:build/layout/paint(persistent 回调)。 */
  persistentCallbacks = 2,
  /** 帧尾回调。 */
  postFrameCallbacks = 3,
}

/** 帧回调, 参数为本帧 vsync 时间(毫秒, 单调时钟)。 */
export type FrameCallback = (timeStampMs: number) => void;

/** {@link SchedulerBinding.scheduleDelayed} 返回的句柄。 */
export interface DelayedTask {
  /** 取消(已执行则无效)。 */
  cancel(): void;
  readonly isActive: boolean;
}

/**
 * 延迟任务的定时器来源。生产环境是宿主的定时器(运行时是 libnode: setTimeout);测试注入假时钟
 * ({@link SchedulerBinding.timerHost}), 时间由测试推进。
 */
export interface TimerHost {
  /** delayMs 毫秒后调用 callback 一次;返回取消用的句柄。 */
  setTimer(callback: () => void, delayMs: number): unknown;
  /** 取消尚未触发的定时器。 */
  clearTimer(handle: unknown): void;
}

/** 宿主定时器。 */
const hostTimers: TimerHost = {
  setTimer: (callback, delayMs) => setTimeout(callback, delayMs),
  clearTimer: (handle) => clearTimeout(handle),
};

class DelayedTaskImpl implements DelayedTask {
  isActive = true;
  readonly #timers: TimerHost;
  readonly #handle: unknown;
  constructor(timers: TimerHost, delayMs: number, callback: () => void, readonly onDone: (task: DelayedTaskImpl) => void) {
    this.#timers = timers;
    this.#handle = timers.setTimer(() => {
      if (!this.isActive) return;
      this.isActive = false;
      this.onDone(this);
      try {
        callback();
      } catch (e) {
        reportError(e, "during a delayed task");
      }
    }, Math.max(0, delayMs));
  }
  cancel(): void {
    if (!this.isActive) return;
    this.isActive = false;
    this.onDone(this);
    this.#timers.clearTimer(this.#handle);
  }
}

/**
 * 帧调度器(对标 Flutter SchedulerBinding)。挂接 platformDispatcher.onBeginFrame/onDrawFrame:
 *   onBeginFrame → transient 回调(Ticker/动画);
 *   onDrawFrame  → persistent 回调(ViewsBinding.drawFrame:build/layout/paint/合成)→ post-frame 回调。
 */
export class SchedulerBinding {
  static #instance: SchedulerBinding | null = null;

  /** 单例(首次访问时初始化并挂接 window 回调)。 */
  static get instance(): SchedulerBinding {
    if (SchedulerBinding.#instance === null) SchedulerBinding.#instance = new SchedulerBinding();
    return SchedulerBinding.#instance;
  }

  #transient = new Map<number, FrameCallback>();
  #transientSwap = new Map<number, FrameCallback>();
  #removedIds = new Set<number>();
  #nextCallbackId = 0;
  #persistent: FrameCallback[] = [];
  #postFrame: FrameCallback[] = [];
  #postFrameSwap: FrameCallback[] = [];
  #hasScheduledFrame = false;
  #phase: SchedulerPhase = SchedulerPhase.idle;
  #currentFrameTimeStamp = 0;
  #framesEnabled = true;
  #frameCount = 0;
  #currentFrameWasRequested = false;
  readonly #delayed = new Set<DelayedTaskImpl>();
  /** 延迟任务用的定时器(缺省宿主定时器;测试换成假时钟)。只影响之后登记的任务。 */
  timerHost: TimerHost = hostTimers;

  private constructor() {
    platformDispatcher.onBeginFrame = (t: number) => this.handleBeginFrame(t);
    platformDispatcher.onDrawFrame = () => this.handleDrawFrame();
  }

  /** 当前阶段。 */
  get schedulerPhase(): SchedulerPhase {
    return this.#phase;
  }
  /** 是否已向引擎请求了下一帧。 */
  get hasScheduledFrame(): boolean {
    return this.#hasScheduledFrame;
  }
  /** 当前(或最近一帧)的 vsync 时间戳(毫秒)。 */
  get currentFrameTimeStamp(): number {
    return this.#currentFrameTimeStamp;
  }
  /**
   * 当前(或最近一帧)是否由框架请求(scheduleFrame)。false: 引擎自己发起的帧(新视图、窗口重新可见、
   * 扩展的帧回调…)。
   */
  get currentFrameWasRequested(): boolean {
    return this.#currentFrameWasRequested;
  }
  /** 已处理的帧数。 */
  get frameCount(): number {
    return this.#frameCount;
  }
  /** 当前登记的 transient 回调数。 */
  get transientCallbackCount(): number {
    return this.#transient.size;
  }
  /** 是否允许出帧(应用挂起时为 false)。 */
  get framesEnabled(): boolean {
    return this.#framesEnabled;
  }
  set framesEnabled(value: boolean) {
    if (this.#framesEnabled === value) return;
    this.#framesEnabled = value;
    if (value) this.scheduleFrame();
  }

  /** 请求一帧(重复调用合并)。 */
  scheduleFrame(): void {
    if (this.#hasScheduledFrame || !this.#framesEnabled) return;
    this.#hasScheduledFrame = true;
    platformDispatcher.scheduleFrame();
  }

  /** 确保会有一帧来处理视觉更新(帧处理中途的请求会在本帧内被消化时不重复请求)。 */
  ensureVisualUpdate(): void {
    if (this.#phase === SchedulerPhase.idle || this.#phase === SchedulerPhase.postFrameCallbacks) this.scheduleFrame();
  }

  /** 登记一次性帧回调(下一帧 onBeginFrame 时调用)并请求帧;返回 id。 */
  scheduleFrameCallback(callback: FrameCallback): number {
    this.scheduleFrame();
    const id = ++this.#nextCallbackId;
    this.#transient.set(id, callback);
    return id;
  }

  /** 取消帧回调。 */
  cancelFrameCallbackWithId(id: number): void {
    if (this.#transient.delete(id)) return;
    this.#removedIds.add(id);
  }

  /** 登记每帧都执行的回调(drawFrame 阶段)。不可移除。 */
  addPersistentFrameCallback(callback: FrameCallback): void {
    this.#persistent.push(callback);
  }

  /** 登记本帧结束后执行一次的回调(不会主动请求帧)。 */
  addPostFrameCallback(callback: FrameCallback): void {
    this.#postFrame.push(callback);
  }

  /**
   * 延迟任务: delayMs 毫秒后调用 callback 一次。由定时器({@link timerHost})驱动, 等待期间不请求帧;
   * 回调在帧外执行, 其中的 setState / markNeedsPaint 等照常请求帧。
   */
  scheduleDelayed(delayMs: number, callback: () => void): DelayedTask {
    const task = new DelayedTaskImpl(this.timerHost, delayMs, callback, (t) => this.#delayed.delete(t));
    this.#delayed.add(task);
    return task;
  }

  /** onBeginFrame 处理:执行 transient 回调。 */
  handleBeginFrame(timeStampMs: number): void {
    this.#currentFrameWasRequested = this.#hasScheduledFrame;
    this.#hasScheduledFrame = false;
    this.#currentFrameTimeStamp = timeStampMs;
    this.#phase = SchedulerPhase.transientCallbacks;
    const callbacks = this.#transient;
    this.#transient = this.#transientSwap;
    this.#transientSwap = callbacks;
    for (const [id, callback] of callbacks) {
      if (this.#removedIds.has(id)) continue;
      try {
        callback(timeStampMs);
      } catch (e) {
        reportError(e, "during a scheduler callback");
      }
    }
    callbacks.clear();
    this.#removedIds.clear();
    this.#phase = SchedulerPhase.idle;
  }

  /** onDrawFrame 处理:persistent 回调后执行 post-frame 回调。 */
  handleDrawFrame(): void {
    const t = this.#currentFrameTimeStamp;
    this.#phase = SchedulerPhase.persistentCallbacks;
    for (let i = 0; i < this.#persistent.length; i++) {
      try {
        this.#persistent[i]!(t);
      } catch (e) {
        reportError(e, "during a persistent frame callback");
      }
    }
    this.#phase = SchedulerPhase.postFrameCallbacks;
    const post = this.#postFrame;
    this.#postFrame = this.#postFrameSwap;
    this.#postFrameSwap = post;
    for (let i = 0; i < post.length; i++) {
      try {
        post[i]!(t);
      } catch (e) {
        reportError(e, "during a post-frame callback");
      }
    }
    post.length = 0;
    this.#phase = SchedulerPhase.idle;
    this.#frameCount++;
  }

  /** 测试用:清空所有 transient/post-frame 回调并取消延迟任务(persistent 保留)。 */
  debugResetForTest(): void {
    for (const task of [...this.#delayed]) task.cancel();
    this.#transient.clear();
    this.#postFrame.length = 0;
    this.#removedIds.clear();
    this.#phase = SchedulerPhase.idle;
    this.#framesEnabled = true;
  }
}
