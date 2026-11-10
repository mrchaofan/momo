/** 缓动曲线:把 [0,1] 映射到(通常)[0,1];transform(0)=0, transform(1)=1。 */
export abstract class Curve {
  /** 变换 t;端点精确返回。 */
  transform(t: number): number {
    if (t === 0 || t === 1) return t;
    return this.transformInternal(t);
  }
  /** 子类实现 (0,1) 内的变换。 */
  protected abstract transformInternal(t: number): number;
  /** 反转曲线:1 - transform(1 - t)。 */
  get flipped(): Curve {
    return new FlippedCurve(this);
  }
}

class LinearCurve extends Curve {
  protected transformInternal(t: number): number {
    return t;
  }
}

/** 三次贝塞尔曲线(控制点 (a,b)、(c,d), 同 CSS cubic-bezier)。 */
export class Cubic extends Curve {
  constructor(readonly a: number, readonly b: number, readonly c: number, readonly d: number) {
    super();
  }
  static #evaluate(a: number, b: number, m: number): number {
    return 3 * a * (1 - m) * (1 - m) * m + 3 * b * (1 - m) * m * m + m * m * m;
  }
  protected transformInternal(t: number): number {
    let start = 0;
    let end = 1;
    for (;;) {
      const mid = (start + end) / 2;
      const estimate = Cubic.#evaluate(this.a, this.c, mid);
      if (Math.abs(t - estimate) < 0.001) return Cubic.#evaluate(this.b, this.d, mid);
      if (estimate < t) start = mid;
      else end = mid;
    }
  }
}

/** 在 [begin, end] 区间内执行 curve, 之前为 0、之后为 1。 */
export class Interval extends Curve {
  constructor(readonly begin: number, readonly end: number, readonly curve: Curve = Curves.linear) {
    super();
  }
  protected transformInternal(t: number): number {
    let x = (t - this.begin) / (this.end - this.begin);
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    return this.curve.transform(x);
  }
}

/** 阶跃:t < threshold 为 0, 否则 1。 */
export class Threshold extends Curve {
  constructor(readonly threshold: number) {
    super();
  }
  protected transformInternal(t: number): number {
    return t < this.threshold ? 0 : 1;
  }
}

/** 反转曲线。 */
export class FlippedCurve extends Curve {
  constructor(readonly curve: Curve) {
    super();
  }
  protected transformInternal(t: number): number {
    return 1 - this.curve.transform(1 - t);
  }
  get flipped(): Curve {
    return this.curve;
  }
}

class DecelerateCurve extends Curve {
  protected transformInternal(t: number): number {
    const u = 1 - t;
    return 1 - u * u;
  }
}

function bounce(t: number): number {
  if (t < 1 / 2.75) return 7.5625 * t * t;
  if (t < 2 / 2.75) {
    t -= 1.5 / 2.75;
    return 7.5625 * t * t + 0.75;
  }
  if (t < 2.5 / 2.75) {
    t -= 2.25 / 2.75;
    return 7.5625 * t * t + 0.9375;
  }
  t -= 2.625 / 2.75;
  return 7.5625 * t * t + 0.984375;
}

class BounceOutCurve extends Curve {
  protected transformInternal(t: number): number {
    return bounce(t);
  }
}

class BounceInCurve extends Curve {
  protected transformInternal(t: number): number {
    return 1 - bounce(1 - t);
  }
}

/** 弹性回弹(越过终点后振荡收敛)。 */
export class ElasticOutCurve extends Curve {
  constructor(readonly period = 0.4) {
    super();
  }
  protected transformInternal(t: number): number {
    const s = this.period / 4;
    return Math.pow(2, -10 * t) * Math.sin(((t - s) * (Math.PI * 2)) / this.period) + 1;
  }
}

/** 常用曲线(与 Flutter Curves 同参数)。 */
export const Curves = {
  linear: new LinearCurve() as Curve,
  decelerate: new DecelerateCurve() as Curve,
  ease: new Cubic(0.25, 0.1, 0.25, 1.0) as Curve,
  easeIn: new Cubic(0.42, 0.0, 1.0, 1.0) as Curve,
  easeOut: new Cubic(0.0, 0.0, 0.58, 1.0) as Curve,
  easeInOut: new Cubic(0.42, 0.0, 0.58, 1.0) as Curve,
  fastOutSlowIn: new Cubic(0.4, 0.0, 0.2, 1.0) as Curve,
  easeInCubic: new Cubic(0.55, 0.055, 0.675, 0.19) as Curve,
  easeOutCubic: new Cubic(0.215, 0.61, 0.355, 1.0) as Curve,
  easeInOutCubic: new Cubic(0.645, 0.045, 0.355, 1.0) as Curve,
  bounceIn: new BounceInCurve() as Curve,
  bounceOut: new BounceOutCurve() as Curve,
  elasticOut: new ElasticOutCurve() as Curve,
};
