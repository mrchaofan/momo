import { Ticker, TickerFuture, defaultTickerProvider, type TickerProvider } from "../scheduler/ticker";
import { AnimationWithLocalListeners, type AnimationStatus } from "./animation";
import { Curves, type Curve } from "./curves";
import { Simulation, SpringDescription, SpringSimulation, Tolerance } from "./simulation";

/** 动画方向。 */
type AnimationDirection = "forward" | "reverse";

/** 系统要求减少动画时的行为:normal 缩短, preserve 保持。 */
export type AnimationBehavior = "normal" | "preserve";

/** AnimationController 构造参数。 */
export interface AnimationControllerOptions {
  /** 初始值(缺省 lowerBound)。 */
  value?: number;
  /** 正向时长(毫秒)。 */
  duration?: number | null;
  /** 反向时长(毫秒);缺省用 duration。 */
  reverseDuration?: number | null;
  lowerBound?: number;
  upperBound?: number;
  /** Ticker 来源;缺省 defaultTickerProvider。 */
  vsync?: TickerProvider;
  debugLabel?: string;
  animationBehavior?: AnimationBehavior;
}

class InterpolationSimulation extends Simulation {
  readonly #begin: number;
  readonly #end: number;
  readonly #durationSec: number;
  readonly #curve: Curve;
  constructor(begin: number, end: number, durationMs: number, curve: Curve) {
    super();
    this.#begin = begin;
    this.#end = end;
    this.#durationSec = durationMs / 1000;
    this.#curve = curve;
  }
  x(time: number): number {
    let t = time / this.#durationSec;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    if (t === 0) return this.#begin;
    if (t === 1) return this.#end;
    return this.#begin + (this.#end - this.#begin) * this.#curve.transform(t);
  }
  dx(time: number): number {
    const eps = this.tolerance.time;
    return (this.x(time + eps) - this.x(time - eps)) / (2 * eps);
  }
  isDone(time: number): boolean {
    return time > this.#durationSec;
  }
}

class RepeatingSimulation extends Simulation {
  readonly #min: number;
  readonly #max: number;
  readonly #reverse: boolean;
  readonly #periodSec: number;
  readonly #initialT: number;
  readonly #setDirection: (d: AnimationDirection) => void;
  constructor(initialValue: number, min: number, max: number, reverse: boolean, periodMs: number,
              setDirection: (d: AnimationDirection) => void) {
    super();
    this.#min = min;
    this.#max = max;
    this.#reverse = reverse;
    this.#periodSec = periodMs / 1000;
    this.#initialT = max === min ? 0 : ((initialValue - min) / (max - min)) * this.#periodSec;
    this.#setDirection = setDirection;
  }
  x(timeInSeconds: number): number {
    const total = timeInSeconds + this.#initialT;
    const cycles = total / this.#periodSec;
    const t = cycles % 1;
    const playingReverse = this.#reverse && Math.floor(cycles) % 2 === 1;
    if (playingReverse) {
      this.#setDirection("reverse");
      return this.#max + (this.#min - this.#max) * t;
    }
    this.#setDirection("forward");
    return this.#min + (this.#max - this.#min) * t;
  }
  dx(): number {
    return (this.#max - this.#min) / this.#periodSec;
  }
  isDone(): boolean {
    return false;
  }
}

const kFlingSpring = SpringDescription.withDampingRatio({ mass: 1, stiffness: 500, ratio: 1 });
const kFlingTolerance = new Tolerance(0.01, 1e-3, 1e-3);

/**
 * 动画控制器(对标 Flutter AnimationController):在 [lowerBound, upperBound] 间按时长/模拟驱动数值,
 * 每帧由 Ticker 推进。时间单位为毫秒。
 */
export class AnimationController extends AnimationWithLocalListeners<number> {
  readonly lowerBound: number;
  readonly upperBound: number;
  readonly debugLabel: string | undefined;
  readonly animationBehavior: AnimationBehavior;
  /** 正向时长(毫秒)。 */
  duration: number | null;
  /** 反向时长(毫秒)。 */
  reverseDuration: number | null;

  #ticker: Ticker | null;
  #value = 0;
  #simulation: Simulation | null = null;
  #direction: AnimationDirection = "forward";
  #status: AnimationStatus = "dismissed";
  #lastReportedStatus: AnimationStatus = "dismissed";
  #lastElapsedMs: number | null = null;

  constructor(opts: AnimationControllerOptions = {}) {
    super();
    this.lowerBound = opts.lowerBound ?? 0;
    this.upperBound = opts.upperBound ?? 1;
    if (!(this.upperBound >= this.lowerBound)) throw new Error("AnimationController: upperBound must be >= lowerBound");
    this.duration = opts.duration ?? null;
    this.reverseDuration = opts.reverseDuration ?? null;
    this.debugLabel = opts.debugLabel;
    this.animationBehavior = opts.animationBehavior ?? "normal";
    this.#ticker = (opts.vsync ?? defaultTickerProvider).createTicker(this.#tick);
    this.#internalSetValue(opts.value ?? this.lowerBound);
    this.#lastReportedStatus = this.#status;
  }

  /** 无界控制器(lowerBound=-∞, upperBound=+∞), 适合物理模拟。 */
  static unbounded(opts: Omit<AnimationControllerOptions, "lowerBound" | "upperBound" | "animationBehavior"> = {}): AnimationController {
    return new AnimationController({ ...opts, value: opts.value ?? 0, lowerBound: -Infinity, upperBound: Infinity, animationBehavior: "preserve" });
  }

  get value(): number {
    return this.#value;
  }
  /** 设置值:停止当前动画并通知。 */
  set value(newValue: number) {
    this.stop();
    this.#internalSetValue(newValue);
    this.notifyListeners();
    this.#checkStatusChanged();
  }

  get status(): AnimationStatus {
    return this.#status;
  }

  /** 是否有 Ticker 在运行。 */
  get isAnimating(): boolean {
    return this.#ticker !== null && this.#ticker.isActive;
  }

  /** 当前速度(单位/秒);未运行时为 0。 */
  get velocity(): number {
    if (!this.isAnimating || this.#simulation === null) return 0;
    return this.#simulation.dx((this.#lastElapsedMs ?? 0) / 1000);
  }

  /** 本次运行已经过的毫秒数;未运行为 null。 */
  get lastElapsedDuration(): number | null {
    return this.#lastElapsedMs;
  }

  #clamp(v: number): number {
    return v < this.lowerBound ? this.lowerBound : v > this.upperBound ? this.upperBound : v;
  }

  #internalSetValue(newValue: number): void {
    this.#value = this.#clamp(newValue);
    if (this.#value === this.lowerBound) this.#status = "dismissed";
    else if (this.#value === this.upperBound) this.#status = "completed";
    else this.#status = this.#direction === "forward" ? "forward" : "reverse";
  }

  /** 回到 lowerBound。 */
  reset(): void {
    this.value = this.lowerBound;
  }

  /** 正向运行到 upperBound。 */
  forward(opts: { from?: number } = {}): TickerFuture {
    this.#direction = "forward";
    if (opts.from !== undefined) this.value = opts.from;
    return this.#animateToInternal(this.upperBound);
  }

  /** 反向运行到 lowerBound。 */
  reverse(opts: { from?: number } = {}): TickerFuture {
    this.#direction = "reverse";
    if (opts.from !== undefined) this.value = opts.from;
    return this.#animateToInternal(this.lowerBound);
  }

  /** 以正向状态运行到 target。 */
  animateTo(target: number, opts: { duration?: number; curve?: Curve } = {}): TickerFuture {
    this.#direction = "forward";
    return this.#animateToInternal(target, opts.duration, opts.curve);
  }

  /** 以反向状态运行到 target。 */
  animateBack(target: number, opts: { duration?: number; curve?: Curve } = {}): TickerFuture {
    this.#direction = "reverse";
    return this.#animateToInternal(target, opts.duration, opts.curve);
  }

  #animateToInternal(target: number, duration?: number, curve: Curve = Curves.linear): TickerFuture {
    let simulationDuration = duration;
    if (simulationDuration === undefined) {
      const range = this.upperBound - this.lowerBound;
      const remainingFraction = Number.isFinite(range) ? Math.abs(target - this.#value) / range : 1;
      const directionDuration =
        this.#direction === "reverse" && this.reverseDuration !== null ? this.reverseDuration : this.duration;
      if (directionDuration === null) {
        throw new Error(`AnimationController${this.debugLabel ? ` (${this.debugLabel})` : ""}: duration is null`);
      }
      simulationDuration = directionDuration * (Number.isFinite(remainingFraction) ? remainingFraction : 0);
    } else if (target === this.#value) {
      simulationDuration = 0;
    }
    this.stop();
    if (simulationDuration === 0) {
      if (this.#value !== target) {
        this.#value = this.#clamp(target);
        this.notifyListeners();
      }
      this.#status = this.#direction === "forward" ? "completed" : "dismissed";
      this.#checkStatusChanged();
      return TickerFuture.complete();
    }
    return this.#startSimulation(new InterpolationSimulation(this.#value, target, simulationDuration, curve));
  }

  /** 在 [min, max] 间循环;reverse=true 时往返。永不结束(需 stop)。 */
  repeat(opts: { min?: number; max?: number; reverse?: boolean; period?: number } = {}): TickerFuture {
    const min = opts.min ?? this.lowerBound;
    const max = opts.max ?? this.upperBound;
    const period = opts.period ?? this.duration;
    if (period === null || period === undefined) throw new Error("AnimationController.repeat: period/duration is null");
    this.stop();
    return this.#startSimulation(
      new RepeatingSimulation(this.#value, min, max, opts.reverse ?? false, period, (d) => {
        this.#direction = d;
        this.#status = d === "forward" ? "forward" : "reverse";
      }),
    );
  }

  /** 以弹簧模拟甩向一端(velocity<0 甩向 lowerBound)。 */
  fling(opts: { velocity?: number; springDescription?: SpringDescription } = {}): TickerFuture {
    const velocity = opts.velocity ?? 1;
    const spring = opts.springDescription ?? kFlingSpring;
    this.#direction = velocity < 0 ? "reverse" : "forward";
    const target = velocity < 0 ? this.lowerBound - kFlingTolerance.distance : this.upperBound + kFlingTolerance.distance;
    const simulation = new SpringSimulation(spring, this.#value, target, velocity, kFlingTolerance);
    this.stop();
    return this.#startSimulation(simulation);
  }

  /** 用任意模拟驱动(正向状态)。 */
  animateWith(simulation: Simulation): TickerFuture {
    this.stop();
    this.#direction = "forward";
    return this.#startSimulation(simulation);
  }

  #startSimulation(simulation: Simulation): TickerFuture {
    const ticker = this.#ticker;
    if (ticker === null) throw new Error("AnimationController used after dispose()");
    this.#simulation = simulation;
    this.#lastElapsedMs = 0;
    this.#value = this.#clamp(simulation.x(0));
    const result = ticker.start();
    this.#status = this.#direction === "forward" ? "forward" : "reverse";
    this.#checkStatusChanged();
    return result;
  }

  /** 停止;canceled(默认 true)决定 TickerFuture 是取消还是完成。 */
  stop(canceled = true): void {
    this.#simulation = null;
    this.#lastElapsedMs = null;
    this.#ticker?.stop(canceled);
  }

  /** 换用新的 TickerProvider(保持当前运行)。 */
  resync(vsync: TickerProvider): void {
    const old = this.#ticker;
    if (old === null) return;
    const ticker = vsync.createTicker(this.#tick);
    ticker.absorbTicker(old);
    this.#ticker = ticker;
  }

  /** 释放:停止 Ticker 并清空监听者。 */
  dispose(): void {
    this.#ticker?.dispose();
    this.#ticker = null;
    this.#simulation = null;
    this.clearListeners();
  }

  #checkStatusChanged(): void {
    const s = this.#status;
    if (this.#lastReportedStatus !== s) {
      this.#lastReportedStatus = s;
      this.notifyStatusListeners(s);
    }
  }

  #tick = (elapsedMs: number): void => {
    const simulation = this.#simulation;
    if (simulation === null) return;
    this.#lastElapsedMs = elapsedMs;
    const t = elapsedMs / 1000;
    this.#value = this.#clamp(simulation.x(t));
    if (simulation.isDone(t)) {
      this.#status = this.#direction === "forward" ? "completed" : "dismissed";
      this.stop(false);
    }
    this.notifyListeners();
    this.#checkStatusChanged();
  };

  toString(): string {
    const label = this.debugLabel ? `${this.debugLabel}; ` : "";
    return `AnimationController(${label}${this.#value.toFixed(3)}; ${this.#status})`;
  }
}
