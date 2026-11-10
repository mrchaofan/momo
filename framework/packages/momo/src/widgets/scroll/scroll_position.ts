import { AnimationController } from "../../animation/controller";
import { Curves, type Curve } from "../../animation/curves";
import { FrictionSimulation } from "../../animation/simulation";
import { ChangeNotifier } from "../../foundation/change_notifier";
import { ViewportOffset } from "../../rendering/viewport_offset";
import { defaultTickerProvider, type TickerProvider } from "../../scheduler/ticker";
import type { Axis } from "../../painting/geometry";

/** 惯性摩擦系数(与 Flutter ClampingScrollPhysics 相近)。 */
const kFrictionDrag = 0.135;
const kMinFlingVelocity = 50;

/**
 * 滚动位置(ViewportOffset 实现):桌面端钳制物理(不越界), 支持 jumpTo/animateTo/滚轮/拖动/惯性。
 */
export class ScrollPosition extends ViewportOffset {
  #pixels: number;
  #min: number | null = null;
  #max: number | null = null;
  #viewport: number | null = null;
  #vsync: TickerProvider;
  #controller: AnimationController | null = null;
  #onTick: (() => void) | null = null;
  readonly axis: Axis;
  /** 是否正在拖动。 */
  isDragging = false;

  constructor(o: { initialPixels?: number; vsync?: TickerProvider; axis?: Axis } = {}) {
    super();
    this.#pixels = o.initialPixels ?? 0;
    this.#vsync = o.vsync ?? defaultTickerProvider;
    this.axis = o.axis ?? "vertical";
  }

  get pixels(): number {
    return this.#pixels;
  }
  get minScrollExtent(): number {
    return this.#min ?? 0;
  }
  get maxScrollExtent(): number {
    return this.#max ?? 0;
  }
  get viewportDimension(): number {
    return this.#viewport ?? 0;
  }
  get hasContentDimensions(): boolean {
    return this.#min !== null && this.#max !== null;
  }
  get extentBefore(): number {
    return Math.max(0, this.#pixels - this.minScrollExtent);
  }
  get extentAfter(): number {
    return Math.max(0, this.maxScrollExtent - this.#pixels);
  }
  get atEdge(): boolean {
    return this.#pixels <= this.minScrollExtent || this.#pixels >= this.maxScrollExtent;
  }
  /** 是否在动画/惯性中。 */
  get isScrollingNotifier(): boolean {
    return this.#controller !== null || this.isDragging;
  }

  #clamp(v: number): number {
    const min = this.#min ?? -Infinity;
    const max = this.#max ?? Infinity;
    return v < min ? min : v > max ? max : v;
  }

  applyViewportDimension(viewportDimension: number): boolean {
    this.#viewport = viewportDimension;
    return true;
  }

  applyContentDimensions(minScrollExtent: number, maxScrollExtent: number): boolean {
    this.#min = minScrollExtent;
    this.#max = Math.max(minScrollExtent, maxScrollExtent);
    const clamped = this.#clamp(this.#pixels);
    if (clamped !== this.#pixels) {
      this.#pixels = clamped;
      return false;
    }
    return true;
  }

  /** 设置像素(钳制), 返回未被接受的量。 */
  setPixels(v: number): number {
    const c = this.#clamp(v);
    if (c !== this.#pixels) {
      this.#pixels = c;
      this.notifyListeners();
    }
    return v - c;
  }

  /** 立即跳转。 */
  jumpTo(v: number): void {
    this.stopActivity();
    this.setPixels(v);
  }

  /** 动画滚动到 v。 */
  animateTo(v: number, o: { duration?: number; curve?: Curve } = {}): Promise<void> {
    const target = this.#clamp(v);
    this.stopActivity();
    if (target === this.#pixels) return Promise.resolve();
    const c = this.#startController(this.#pixels);
    return Promise.resolve(c.animateTo(target, { duration: o.duration ?? 300, curve: o.curve ?? Curves.easeInOut }).orCancel.catch(() => undefined))
      .then(() => this.#finishController(c));
  }

  #startController(from: number): AnimationController {
    const c = AnimationController.unbounded({ value: from, vsync: this.#vsync, debugLabel: "ScrollPosition" });
    this.#onTick = () => {
      const over = this.setPixels(c.value);
      if (over !== 0) this.stopActivity();
    };
    c.addListener(this.#onTick);
    this.#controller = c;
    return c;
  }

  #finishController(c: AnimationController): void {
    if (this.#controller === c) {
      this.#controller = null;
      if (this.#onTick) c.removeListener(this.#onTick);
      this.#onTick = null;
    }
    c.dispose();
  }

  /** 停止动画/惯性。 */
  stopActivity(): void {
    const c = this.#controller;
    if (c !== null) {
      this.#controller = null;
      if (this.#onTick) c.removeListener(this.#onTick);
      this.#onTick = null;
      c.stop();
      c.dispose();
    }
  }

  /** 滚轮/触控板位移(正值向内容末尾)。返回是否发生了滚动。 */
  pointerScroll(delta: number): boolean {
    if (delta === 0) return false;
    const target = this.#clamp(this.#pixels + delta);
    if (target === this.#pixels) return false;
    this.stopActivity();
    this.setPixels(target);
    return true;
  }

  /** 拖动开始。 */
  dragStart(): void {
    this.stopActivity();
    this.isDragging = true;
  }
  /** 拖动更新:手指位移 delta(向下为正 → 内容上移 → pixels 减小)。 */
  dragUpdate(delta: number): void {
    this.setPixels(this.#pixels - delta);
  }
  /** 拖动结束(手指速度, 逻辑像素/秒)。 */
  dragEnd(velocity: number): void {
    this.isDragging = false;
    this.goBallistic(-velocity);
  }
  dragCancel(): void {
    this.isDragging = false;
  }

  /** 以 pixels 速度(像素/秒)惯性滚动。 */
  goBallistic(velocity: number): void {
    this.stopActivity();
    if (Math.abs(velocity) < kMinFlingVelocity || this.atEdge && ((velocity < 0 && this.#pixels <= this.minScrollExtent) || (velocity > 0 && this.#pixels >= this.maxScrollExtent))) return;
    const sim = new FrictionSimulation(kFrictionDrag, this.#pixels, velocity);
    const c = this.#startController(this.#pixels);
    void Promise.resolve(c.animateWith(sim).orCancel.catch(() => undefined)).then(() => this.#finishController(c));
  }

  override dispose(): void {
    this.stopActivity();
    super.dispose();
  }
}

/** 滚动控制器:创建并持有 ScrollPosition, 也可从外部 jumpTo/animateTo。 */
export class ScrollController extends ChangeNotifier {
  readonly initialScrollOffset: number;
  #positions: ScrollPosition[] = [];
  #forward = (): void => this.notifyListeners();

  constructor(o: { initialScrollOffset?: number } = {}) {
    super();
    this.initialScrollOffset = o.initialScrollOffset ?? 0;
  }

  get hasClients(): boolean {
    return this.#positions.length > 0;
  }
  get position(): ScrollPosition {
    const p = this.#positions[0];
    if (p === undefined) throw new Error("ScrollController not attached to any scroll views.");
    return p;
  }
  get offset(): number {
    return this.position.pixels;
  }

  createScrollPosition(vsync: TickerProvider, axis: Axis): ScrollPosition {
    return new ScrollPosition({ initialPixels: this.initialScrollOffset, vsync, axis });
  }
  attach(p: ScrollPosition): void {
    this.#positions.push(p);
    p.addListener(this.#forward);
  }
  detach(p: ScrollPosition): void {
    p.removeListener(this.#forward);
    const i = this.#positions.indexOf(p);
    if (i >= 0) this.#positions.splice(i, 1);
  }
  jumpTo(v: number): void {
    for (const p of this.#positions) p.jumpTo(v);
  }
  animateTo(v: number, o: { duration?: number; curve?: Curve } = {}): Promise<void> {
    return Promise.all(this.#positions.map((p) => p.animateTo(v, o))).then(() => undefined);
  }
  override dispose(): void {
    for (const p of this.#positions) p.removeListener(this.#forward);
    this.#positions = [];
    super.dispose();
  }
}
