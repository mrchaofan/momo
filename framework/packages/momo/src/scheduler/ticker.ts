import { SchedulerBinding } from "./binding";

/** Ticker 每帧回调, 参数为自 start 起经过的毫秒数。 */
export type TickerCallback = (elapsedMs: number) => void;

/** 创建 Ticker 的对象(State 等)。 */
export interface TickerProvider {
  createTicker(onTick: TickerCallback): Ticker;
}

/** Ticker 被取消(stop(true))时 {@link TickerFuture.orCancel} 抛出的错误。 */
export class TickerCanceled extends Error {
  constructor() {
    super("The ticker was canceled before the operation completed.");
    this.name = "TickerCanceled";
  }
}

/**
 * Ticker 一次运行的结果:正常停止时完成;取消时 {@link orCancel} 以 {@link TickerCanceled} 拒绝,
 * 而本身永不完成(与 Flutter 一致)。
 */
export class TickerFuture implements PromiseLike<void> {
  #completed = false;
  #canceled = false;
  #resolve: (() => void) | null = null;
  #promise: Promise<void>;
  #cancelPromise: Promise<void> | null = null;
  #rejectCancel: ((e: unknown) => void) | null = null;
  #resolveCancel: (() => void) | null = null;

  constructor() {
    this.#promise = new Promise<void>((resolve) => {
      this.#resolve = resolve;
    });
  }

  /** 立即完成的 future。 */
  static complete(): TickerFuture {
    const f = new TickerFuture();
    f.complete();
    return f;
  }

  get isCompleted(): boolean {
    return this.#completed;
  }

  /** @internal */
  complete(): void {
    if (this.#completed || this.#canceled) return;
    this.#completed = true;
    this.#resolve?.();
    this.#resolveCancel?.();
  }

  /** @internal */
  cancel(): void {
    if (this.#completed || this.#canceled) return;
    this.#canceled = true;
    this.#rejectCancel?.(new TickerCanceled());
  }

  /** 正常完成或被取消时都结束(取消时拒绝)。 */
  get orCancel(): Promise<void> {
    if (this.#cancelPromise === null) {
      this.#cancelPromise = new Promise<void>((resolve, reject) => {
        if (this.#completed) resolve();
        else if (this.#canceled) reject(new TickerCanceled());
        else {
          this.#resolveCancel = resolve;
          this.#rejectCancel = reject;
        }
      });
    }
    return this.#cancelPromise;
  }

  then<R1 = void, R2 = never>(
    onfulfilled?: ((value: void) => R1 | PromiseLike<R1>) | null,
    onrejected?: ((reason: unknown) => R2 | PromiseLike<R2>) | null,
  ): Promise<R1 | R2> {
    return this.#promise.then(onfulfilled, onrejected);
  }
}

/**
 * 每帧调用一次回调的计时器(对标 Flutter Ticker)。基于 {@link SchedulerBinding.scheduleFrameCallback}。
 */
export class Ticker {
  #onTick: TickerCallback | null;
  #future: TickerFuture | null = null;
  #startTime = -1;
  #callbackId = 0;
  #muted = false;
  #disposed = false;
  /** @internal 释放时通知所属 TickerProvider(如 State)把自己移出集合。 */
  _onDispose: ((ticker: Ticker) => void) | null = null;

  constructor(onTick: TickerCallback, readonly debugLabel?: string) {
    this.#onTick = onTick;
  }

  /** 是否已 start 且未 stop。 */
  get isActive(): boolean {
    return this.#future !== null;
  }
  /** 是否正在实际回调(active 且未静音)。 */
  get isTicking(): boolean {
    return this.#future !== null && !this.#muted;
  }
  /** 静音时不回调但时间继续流逝。 */
  get muted(): boolean {
    return this.#muted;
  }
  set muted(value: boolean) {
    if (value === this.#muted) return;
    this.#muted = value;
    if (value) this.#unscheduleTick();
    else if (this.#future !== null) this.#scheduleTick();
  }

  /** 开始计时;返回本次运行的 future。 */
  start(): TickerFuture {
    if (this.#disposed) throw new Error("Ticker.start() called after dispose()");
    if (this.#future !== null) throw new Error("Ticker.start() called while already active");
    this.#future = new TickerFuture();
    this.#startTime = -1;
    if (!this.#muted) this.#scheduleTick();
    return this.#future;
  }

  /** 停止;canceled=true 时 future 视为取消。 */
  stop(canceled = false): void {
    const future = this.#future;
    if (future === null) return;
    this.#future = null;
    this.#startTime = -1;
    this.#unscheduleTick();
    if (canceled) future.cancel();
    else future.complete();
  }

  /** 是否已释放。 */
  get isDisposed(): boolean {
    return this.#disposed;
  }

  /** 释放(会取消当前运行)。幂等;释放后断开回调, 并通知所属 provider 移除自身。 */
  dispose(): void {
    if (this.#disposed) return;
    this.stop(true);
    this.#disposed = true;
    this.#onTick = null;
    const onDispose = this._onDispose;
    this._onDispose = null;
    onDispose?.(this);
  }

  /** 把另一个 Ticker 的运行状态(future 与起始时间)接管过来。 */
  absorbTicker(original: Ticker): void {
    if (original.#future !== null) {
      this.#future = original.#future;
      this.#startTime = original.#startTime;
      if (!this.#muted) this.#scheduleTick();
      original.#future = null;
      original.#unscheduleTick();
    }
    original.dispose();
  }

  #tick = (timeStampMs: number): void => {
    this.#callbackId = 0;
    if (this.#future === null) return;
    if (this.#startTime < 0) this.#startTime = timeStampMs;
    this.#onTick?.(timeStampMs - this.#startTime);
    if (this.#future !== null && !this.#muted && this.#callbackId === 0) this.#scheduleTick();
  };

  #scheduleTick(): void {
    if (this.#callbackId !== 0) return;
    this.#callbackId = SchedulerBinding.instance.scheduleFrameCallback(this.#tick);
  }

  #unscheduleTick(): void {
    if (this.#callbackId !== 0) {
      SchedulerBinding.instance.cancelFrameCallbackWithId(this.#callbackId);
      this.#callbackId = 0;
    }
  }
}

/** 默认 TickerProvider:直接创建独立 Ticker。 */
export const defaultTickerProvider: TickerProvider = {
  createTicker(onTick: TickerCallback): Ticker {
    return new Ticker(onTick);
  },
};
