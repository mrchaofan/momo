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
import { State, StatefulWidget, type BuildContext, type Widget } from "./framework";
import type { Alignment } from "../painting/alignment";
import type { Decoration } from "../painting/decoration";

/** 随 Listenable 重建的 widget 基类。 */
export abstract class AnimatedWidget extends StatefulWidget {
  readonly listenable: Listenable;
  constructor(o: { key?: Key | null; listenable: Listenable }) {
    super(o.key);
    this.listenable = o.listenable;
  }
  /** 构建(listenable 每次通知时重建)。 */
  abstract build(context: BuildContext): Widget;
  createState(): State {
    return new AnimatedState();
  }
}

class AnimatedState extends State<AnimatedWidget> {
  #onChange = (): void => this.setState();
  override initState(): void {
    this.widget.listenable.addListener(this.#onChange);
  }
  override didUpdateWidget(old: AnimatedWidget): void {
    if (old.listenable !== this.widget.listenable) {
      old.listenable.removeListener(this.#onChange);
      this.widget.listenable.addListener(this.#onChange);
    }
  }
  override dispose(): void {
    this.widget.listenable.removeListener(this.#onChange);
  }
  build(context: BuildContext): Widget {
    return this.widget.build(context);
  }
}

/** 以 builder 随动画重建;child 不随之重建(性能优化)。 */
export class AnimatedBuilder extends AnimatedWidget {
  readonly builder: (context: BuildContext, child: Widget | null) => Widget;
  readonly child: Widget | null;
  constructor(o: { key?: Key | null; animation: Listenable; builder: (context: BuildContext, child: Widget | null) => Widget; child?: Widget | null }) {
    super({ key: o.key ?? null, listenable: o.animation });
    this.builder = o.builder;
    this.child = o.child ?? null;
  }
  build(context: BuildContext): Widget {
    return this.builder(context, this.child);
  }
}

/** 随 ValueListenable 的值重建。 */
export class ValueListenableBuilder<T> extends AnimatedWidget {
  readonly valueListenable: ValueListenable<T>;
  readonly builder: (context: BuildContext, value: T, child: Widget | null) => Widget;
  readonly child: Widget | null;
  constructor(o: { key?: Key | null; valueListenable: ValueListenable<T>; builder: (context: BuildContext, value: T, child: Widget | null) => Widget; child?: Widget | null }) {
    super({ key: o.key ?? null, listenable: o.valueListenable });
    this.valueListenable = o.valueListenable;
    this.builder = o.builder;
    this.child = o.child ?? null;
  }
  build(context: BuildContext): Widget {
    return this.builder(context, this.valueListenable.value, this.child);
  }
}

/** 透明度过渡(由 Animation<number> 驱动)。 */
export class FadeTransition extends AnimatedWidget {
  readonly opacity: Animation<number>;
  readonly child: Widget | null;
  constructor(o: { key?: Key | null; opacity: Animation<number>; child?: Widget | null }) {
    super({ key: o.key ?? null, listenable: o.opacity });
    this.opacity = o.opacity;
    this.child = o.child ?? null;
  }
  build(): Widget {
    return new Opacity({ opacity: this.opacity.value, child: this.child });
  }
}

/** 隐式动画基类:属性变化时自动从当前值动画到新值。 */
export abstract class ImplicitlyAnimatedWidget extends StatefulWidget {
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
export abstract class ImplicitlyAnimatedWidgetState<W extends ImplicitlyAnimatedWidget> extends State<W> {
  #controller: AnimationController | null = null;
  #animation: CurvedAnimation | null = null;

  get controller(): AnimationController {
    return this.#controller!;
  }
  get animation(): Animation<number> {
    return this.#animation!;
  }

  override initState(): void {
    const c = new AnimationController({ duration: this.widget.duration, vsync: this });
    this.#controller = c;
    this.#animation = new CurvedAnimation({ parent: c, curve: this.widget.curve });
    c.addStatusListener((s) => {
      if (s === "completed") this.widget.onEnd?.();
    });
    c.addListener(() => this.setState());
    this.forEachTween((tween, target) => {
      tween.begin = target;
      tween.end = target;
    });
  }

  override didUpdateWidget(old: W): void {
    const c = this.controller;
    if (old.curve !== this.widget.curve) {
      this.#animation?.dispose();
      this.#animation = new CurvedAnimation({ parent: c, curve: this.widget.curve });
    }
    c.duration = this.widget.duration;
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
export class AnimatedOpacity extends ImplicitlyAnimatedWidget {
  readonly opacity: number;
  readonly child: Widget | null;
  constructor(o: { key?: Key | null; opacity: number; duration: number; curve?: Curve; onEnd?: (() => void) | null; child?: Widget | null }) {
    super(o);
    this.opacity = o.opacity;
    this.child = o.child ?? null;
  }
  createState(): State {
    return new AnimatedOpacityState();
  }
}

class AnimatedOpacityState extends ImplicitlyAnimatedWidgetState<AnimatedOpacity> {
  readonly #tween = new Tween<number>(0, 0);
  protected forEachTween(visitor: (tween: Tween<number>, target: number) => void): void {
    visitor(this.#tween, this.widget.opacity);
  }
  build(): Widget {
    return new Opacity({ opacity: this.#tween.transform(this.animation.value), child: this.widget.child });
  }
}

/** 尺寸/颜色/内边距隐式动画容器(简化版:width/height/color/padding)。 */
export class AnimatedContainer extends ImplicitlyAnimatedWidget {
  readonly width: number | null;
  readonly height: number | null;
  readonly color: Color | null;
  readonly padding: EdgeInsets | null;
  readonly alignment: Alignment | null;
  readonly decoration: Decoration | null;
  readonly child: Widget | null;
  constructor(o: { key?: Key | null; duration: number; curve?: Curve; onEnd?: (() => void) | null; width?: number | null; height?: number | null; color?: Color | null; padding?: EdgeInsets | null; alignment?: Alignment | null; decoration?: Decoration | null; child?: Widget | null }) {
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

class AnimatedContainerState extends ImplicitlyAnimatedWidgetState<AnimatedContainer> {
  readonly #w = new Tween<number>(0, 0);
  readonly #h = new Tween<number>(0, 0);
  readonly #pad = [0, 0, 0, 0].map(() => new Tween<number>(0, 0));
  readonly #argb = [0, 0, 0, 0].map(() => new Tween<number>(0, 0));
  protected forEachTween(visitor: (tween: Tween<number>, target: number) => void): void {
    const w = this.widget;
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
  build(): Widget {
    const w = this.widget;
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
