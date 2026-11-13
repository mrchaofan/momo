import { AnimationController } from "../animation/controller";
import { CurvedAnimation } from "../animation/animations";
import { Curves, type Curve } from "../animation/curves";
import { Tween } from "../animation/tween";
import type { Animation } from "../animation/animation";
import type { Listenable, ValueListenable } from "../foundation/change_notifier";
import type { Key } from "../foundation/key";
import type { Color } from "../painting/color";
import { Opacity } from "./basic";
import { Container } from "./container";
import type { EdgeInsets } from "../painting/edge_insets";
import { EdgeInsets as EI } from "../painting/edge_insets";
import { Color as C } from "../painting/color";
import { State, StatefulView, type BuildContext, type View } from "./framework";
import type { Alignment } from "../painting/alignment";
import type { Decoration } from "../painting/decoration";

/** 随 Listenable 重建的 view 基类。 */
export abstract class AnimatedView extends StatefulView {
  readonly listenable: Listenable;
  constructor(o: { key?: Key | null; listenable: Listenable }) {
    super(o.key);
    this.listenable = o.listenable;
  }
  /** 构建(listenable 每次通知时重建)。 */
  abstract build(context: BuildContext): View;
  createState(): State {
    return new AnimatedState();
  }
}

class AnimatedState extends State<AnimatedView> {
  #onChange = (): void => this.setState();
  override initState(): void {
    this.view.listenable.addListener(this.#onChange);
  }
  override didUpdateView(old: AnimatedView): void {
    if (old.listenable !== this.view.listenable) {
      old.listenable.removeListener(this.#onChange);
      this.view.listenable.addListener(this.#onChange);
    }
  }
  override dispose(): void {
    this.view.listenable.removeListener(this.#onChange);
  }
  build(context: BuildContext): View {
    return this.view.build(context);
  }
}

/** 以 builder 随动画重建;child 不随之重建(性能优化)。 */
export class AnimatedBuilder extends AnimatedView {
  readonly builder: (context: BuildContext, child: View | null) => View;
  readonly child: View | null;
  constructor(o: { key?: Key | null; animation: Listenable; builder: (context: BuildContext, child: View | null) => View; child?: View | null }) {
    super({ key: o.key ?? null, listenable: o.animation });
    this.builder = o.builder;
    this.child = o.child ?? null;
  }
  build(context: BuildContext): View {
    return this.builder(context, this.child);
  }
}

/** 随 ValueListenable 的值重建。 */
export class ValueListenableBuilder<T> extends AnimatedView {
  readonly valueListenable: ValueListenable<T>;
  readonly builder: (context: BuildContext, value: T, child: View | null) => View;
  readonly child: View | null;
  constructor(o: { key?: Key | null; valueListenable: ValueListenable<T>; builder: (context: BuildContext, value: T, child: View | null) => View; child?: View | null }) {
    super({ key: o.key ?? null, listenable: o.valueListenable });
    this.valueListenable = o.valueListenable;
    this.builder = o.builder;
    this.child = o.child ?? null;
  }
  build(context: BuildContext): View {
    return this.builder(context, this.valueListenable.value, this.child);
  }
}

/** 透明度过渡(由 Animation<number> 驱动)。 */
export class FadeTransition extends AnimatedView {
  readonly opacity: Animation<number>;
  readonly child: View | null;
  constructor(o: { key?: Key | null; opacity: Animation<number>; child?: View | null }) {
    super({ key: o.key ?? null, listenable: o.opacity });
    this.opacity = o.opacity;
    this.child = o.child ?? null;
  }
  build(): View {
    return new Opacity({ opacity: this.opacity.value, child: this.child });
  }
}

/** 隐式动画基类:属性变化时自动从当前值动画到新值。 */
export abstract class ImplicitlyAnimatedView extends StatefulView {
  readonly duration: number;
  readonly curve: Curve;
  readonly onEnd: (() => void) | null;
  constructor(o: { key?: Key | null; duration: number; curve?: Curve; onEnd?: (() => void) | null }) {
    super(o.key);
    this.duration = o.duration;
    this.curve = o.curve ?? Curves.linear;
    this.onEnd = o.onEnd ?? null;
  }
}

/** 隐式动画 State:子类在 forEachTween 中声明要插值的属性。 */
export abstract class ImplicitlyAnimatedViewState<W extends ImplicitlyAnimatedView> extends State<W> {
  #controller: AnimationController | null = null;
  #animation: CurvedAnimation | null = null;

  get controller(): AnimationController {
    return this.#controller!;
  }
  get animation(): Animation<number> {
    return this.#animation!;
  }

  override initState(): void {
    const c = new AnimationController({ duration: this.view.duration, vsync: this });
    this.#controller = c;
    this.#animation = new CurvedAnimation({ parent: c, curve: this.view.curve });
    c.addStatusListener((s) => {
      if (s === "completed") this.view.onEnd?.();
    });
    c.addListener(() => this.setState());
    this.forEachTween((tween, target) => {
      tween.begin = target;
      tween.end = target;
    });
  }

  override didUpdateView(old: W): void {
    const c = this.controller;
    if (old.curve !== this.view.curve) {
      this.#animation?.dispose();
      this.#animation = new CurvedAnimation({ parent: c, curve: this.view.curve });
    }
    c.duration = this.view.duration;
    let changed = false;
    this.forEachTween((tween, target) => {
      if (tween.end !== target) {
        tween.begin = tween.transform(this.animation.value);
        tween.end = target;
        changed = true;
      }
    });
    if (changed) {
      c.value = 0;
      c.forward();
    }
  }

  override dispose(): void {
    this.#animation?.dispose();
    this.#controller?.dispose();
  }

  /** 遍历每个属性的 tween 与目标值。 */
  protected abstract forEachTween(visitor: (tween: Tween<number>, target: number) => void): void;
}

/** 透明度隐式动画。 */
export class AnimatedOpacity extends ImplicitlyAnimatedView {
  readonly opacity: number;
  readonly child: View | null;
  constructor(o: { key?: Key | null; opacity: number; duration: number; curve?: Curve; onEnd?: (() => void) | null; child?: View | null }) {
    super(o);
    this.opacity = o.opacity;
    this.child = o.child ?? null;
  }
  createState(): State {
    return new AnimatedOpacityState();
  }
}

class AnimatedOpacityState extends ImplicitlyAnimatedViewState<AnimatedOpacity> {
  readonly #tween = new Tween<number>(0, 0);
  protected forEachTween(visitor: (tween: Tween<number>, target: number) => void): void {
    visitor(this.#tween, this.view.opacity);
  }
  build(): View {
    return new Opacity({ opacity: this.#tween.transform(this.animation.value), child: this.view.child });
  }
}

/** 尺寸/颜色/内边距隐式动画容器(简化版:width/height/color/padding)。 */
export class AnimatedContainer extends ImplicitlyAnimatedView {
  readonly width: number | null;
  readonly height: number | null;
  readonly color: Color | null;
  readonly padding: EdgeInsets | null;
  readonly alignment: Alignment | null;
  readonly decoration: Decoration | null;
  readonly child: View | null;
  constructor(o: { key?: Key | null; duration: number; curve?: Curve; onEnd?: (() => void) | null; width?: number | null; height?: number | null; color?: Color | null; padding?: EdgeInsets | null; alignment?: Alignment | null; decoration?: Decoration | null; child?: View | null }) {
    super(o);
    this.width = o.width ?? null;
    this.height = o.height ?? null;
    this.color = o.color ?? null;
    this.padding = o.padding ?? null;
    this.alignment = o.alignment ?? null;
    this.decoration = o.decoration ?? null;
    this.child = o.child ?? null;
  }
  createState(): State {
    return new AnimatedContainerState();
  }
}

class AnimatedContainerState extends ImplicitlyAnimatedViewState<AnimatedContainer> {
  readonly #w = new Tween<number>(0, 0);
  readonly #h = new Tween<number>(0, 0);
  readonly #pad = [0, 0, 0, 0].map(() => new Tween<number>(0, 0));
  readonly #argb = [0, 0, 0, 0].map(() => new Tween<number>(0, 0));
  protected forEachTween(visitor: (tween: Tween<number>, target: number) => void): void {
    const w = this.view;
    if (w.width !== null) visitor(this.#w, w.width);
    if (w.height !== null) visitor(this.#h, w.height);
    if (w.padding !== null) {
      const p = [w.padding.left, w.padding.top, w.padding.right, w.padding.bottom];
      this.#pad.forEach((t, i) => visitor(t, p[i]!));
    }
    if (w.color !== null) {
      const c = [w.color.alpha, w.color.red, w.color.green, w.color.blue];
      this.#argb.forEach((t, i) => visitor(t, c[i]!));
    }
  }
  build(): View {
    const w = this.view;
    const t = this.animation.value;
    const ch = (i: number): number => Math.round(Math.min(255, Math.max(0, this.#argb[i]!.transform(t))));
    return new Container({
      width: w.width !== null ? this.#w.transform(t) : null,
      height: w.height !== null ? this.#h.transform(t) : null,
      padding: w.padding !== null ? new EI(this.#pad[0]!.transform(t), this.#pad[1]!.transform(t), this.#pad[2]!.transform(t), this.#pad[3]!.transform(t)) : null,
      color: w.color !== null ? C.fromARGB(ch(0), ch(1), ch(2), ch(3)) : null,
      alignment: w.alignment,
      decoration: w.decoration,
      child: w.child,
    });
  }
}
