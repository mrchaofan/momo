import { Color } from "../painting/color";
import { Offset, Rect, Size } from "../painting/geometry";
import { Animation, AnimationWithParent } from "./animation";
import type { Curve } from "./curves";

/** 把 t(通常 0..1)映射为 T 的对象。 */
export abstract class Animatable<T> {
  abstract transform(t: number): T;
  /** 以动画当前值求值。 */
  evaluate(animation: Animation<number>): T {
    return this.transform(animation.value);
  }
  /** 生成由 parent 驱动的动画。 */
  animate(parent: Animation<number>): Animation<T> {
    return new AnimatedEvaluation(parent, this);
  }
  /** 先经 parent 变换再经本对象变换。 */
  chain(parent: Animatable<number>): Animatable<T> {
    return new ChainedEvaluation(parent, this);
  }
}

class AnimatedEvaluation<T> extends AnimationWithParent<T> {
  constructor(parent: Animation<number>, readonly evaluatable: Animatable<T>) {
    super(parent);
  }
  get value(): T {
    return this.evaluatable.evaluate(this.parent);
  }
}

class ChainedEvaluation<T> extends Animatable<T> {
  constructor(readonly parentAnimatable: Animatable<number>, readonly evaluatable: Animatable<T>) {
    super();
  }
  transform(t: number): T {
    return this.evaluatable.transform(this.parentAnimatable.transform(t));
  }
}

/**
 * begin → end 的线性插值。数字类型有默认 lerp;其他类型请使用子类(ColorTween 等)或覆盖 lerp。
 */
export class Tween<T> extends Animatable<T> {
  constructor(public begin: T, public end: T) {
    super();
  }
  /** t 处的插值(t=0/1 由 transform 直接返回端点)。 */
  lerp(t: number): T {
    const b: unknown = this.begin;
    const e: unknown = this.end;
    if (typeof b === "number" && typeof e === "number") return (b + (e - b) * t) as unknown as T;
    throw new Error(`${this.constructor.name}: cannot lerp non-numeric values; override lerp()`);
  }
  transform(t: number): T {
    if (t === 0) return this.begin;
    if (t === 1) return this.end;
    return this.lerp(t);
  }
  toString(): string {
    return `${this.constructor.name}(${String(this.begin)} → ${String(this.end)})`;
  }
}

/** 反向插值:end → begin。 */
export class ReverseTween<T> extends Tween<T> {
  constructor(readonly parentTween: Tween<T>) {
    super(parentTween.end, parentTween.begin);
  }
  lerp(t: number): T {
    return this.parentTween.lerp(1 - t);
  }
}

/** 颜色插值(Color.lerp, null 视为透明)。 */
export class ColorTween extends Tween<Color | null> {
  lerp(t: number): Color | null {
    return Color.lerp(this.begin, this.end, t);
  }
}

/** 尺寸插值。 */
export class SizeTween extends Tween<Size> {
  lerp(t: number): Size {
    return Size.lerp(this.begin, this.end, t);
  }
}

/** 矩形插值。 */
export class RectTween extends Tween<Rect> {
  lerp(t: number): Rect {
    return Rect.lerp(this.begin, this.end, t);
  }
}

/** 偏移插值。 */
export class OffsetTween extends Tween<Offset> {
  lerp(t: number): Offset {
    return Offset.lerp(this.begin, this.end, t);
  }
}

/** 整数插值(四舍五入)。 */
export class IntTween extends Tween<number> {
  lerp(t: number): number {
    return Math.round(this.begin + (this.end - this.begin) * t);
  }
}

/** 常量(begin = end = value)。 */
export class ConstantTween<T> extends Tween<T> {
  constructor(value: T) {
    super(value, value);
  }
  lerp(): T {
    return this.begin;
  }
}

/** 以曲线变换 t。 */
export class CurveTween extends Animatable<number> {
  constructor(public curve: Curve) {
    super();
  }
  transform(t: number): number {
    if (t === 0 || t === 1) return t;
    return this.curve.transform(t);
  }
}

/** TweenSequence 的一段。 */
export interface TweenSequenceItem<T> {
  tween: Animatable<T>;
  /** 本段占总时长的权重。 */
  weight: number;
}

/** 按权重依次拼接多个 Animatable。 */
export class TweenSequence<T> extends Animatable<T> {
  readonly #items: TweenSequenceItem<T>[];
  readonly #starts: number[] = [];
  readonly #ends: number[] = [];

  constructor(items: TweenSequenceItem<T>[]) {
    super();
    if (items.length === 0) throw new Error("TweenSequence requires at least one item");
    this.#items = items.slice();
    let total = 0;
    for (const i of items) total += i.weight;
    let start = 0;
    for (const i of items) {
      const end = start + i.weight / total;
      this.#starts.push(start);
      this.#ends.push(end);
      start = end;
    }
  }

  transform(t: number): T {
    const n = this.#items.length;
    if (t >= 1) return this.#items[n - 1]!.tween.transform(1);
    for (let i = 0; i < n; i++) {
      const s = this.#starts[i]!;
      const e = this.#ends[i]!;
      if (t >= s && t < e) return this.#items[i]!.tween.transform((t - s) / (e - s));
    }
    return this.#items[0]!.tween.transform(0);
  }
}
