/** 判定"足够接近"的容差。 */
export class Tolerance {
  static readonly defaultTolerance = new Tolerance();
  constructor(readonly distance = 1e-3, readonly time = 1e-3, readonly velocity = 1e-3) {}
}

/** |a - b| <= epsilon。 */
export function nearEqual(a: number, b: number, epsilon: number): boolean {
  return Math.abs(a - b) <= epsilon || a === b;
}

/** |a| <= epsilon。 */
export function nearZero(a: number, epsilon: number): boolean {
  return nearEqual(a, 0, epsilon);
}

/** 一维物理模拟:位置 x(t)、速度 dx(t), t 单位为秒。 */
export abstract class Simulation {
  tolerance: Tolerance;
  constructor(tolerance: Tolerance = Tolerance.defaultTolerance) {
    this.tolerance = tolerance;
  }
  abstract x(time: number): number;
  abstract dx(time: number): number;
  abstract isDone(time: number): boolean;
}

/** 弹簧参数。 */
export class SpringDescription {
  constructor(readonly mass: number, readonly stiffness: number, readonly damping: number) {}
  /** 由阻尼比构造:damping = ratio · 2√(mass·stiffness)。ratio=1 为临界阻尼。 */
  static withDampingRatio(opts: { mass: number; stiffness: number; ratio?: number }): SpringDescription {
    const ratio = opts.ratio ?? 1;
    return new SpringDescription(opts.mass, opts.stiffness, ratio * 2 * Math.sqrt(opts.mass * opts.stiffness));
  }
}

/** 弹簧解的类型。 */
export type SpringType = "criticallyDamped" | "underDamped" | "overDamped";

interface SpringSolution {
  readonly type: SpringType;
  x(t: number): number;
  dx(t: number): number;
}

class CriticalSolution implements SpringSolution {
  readonly type = "criticallyDamped" as const;
  readonly #r: number;
  readonly #c1: number;
  readonly #c2: number;
  constructor(spring: SpringDescription, distance: number, velocity: number) {
    this.#r = -spring.damping / (2 * spring.mass);
    this.#c1 = distance;
    this.#c2 = velocity - this.#r * distance;
  }
  x(t: number): number {
    return (this.#c1 + this.#c2 * t) * Math.exp(this.#r * t);
  }
  dx(t: number): number {
    const e = Math.exp(this.#r * t);
    return e * (this.#c2 + this.#r * (this.#c1 + this.#c2 * t));
  }
}

class OverdampedSolution implements SpringSolution {
  readonly type = "overDamped" as const;
  readonly #r1: number;
  readonly #r2: number;
  readonly #c1: number;
  readonly #c2: number;
  constructor(spring: SpringDescription, distance: number, velocity: number, cmk: number) {
    const root = Math.sqrt(cmk);
    this.#r1 = (-spring.damping - root) / (2 * spring.mass);
    this.#r2 = (-spring.damping + root) / (2 * spring.mass);
    this.#c2 = (velocity - this.#r1 * distance) / (this.#r2 - this.#r1);
    this.#c1 = distance - this.#c2;
  }
  x(t: number): number {
    return this.#c1 * Math.exp(this.#r1 * t) + this.#c2 * Math.exp(this.#r2 * t);
  }
  dx(t: number): number {
    return this.#c1 * this.#r1 * Math.exp(this.#r1 * t) + this.#c2 * this.#r2 * Math.exp(this.#r2 * t);
  }
}

class UnderdampedSolution implements SpringSolution {
  readonly type = "underDamped" as const;
  readonly #w: number;
  readonly #r: number;
  readonly #c1: number;
  readonly #c2: number;
  constructor(spring: SpringDescription, distance: number, velocity: number, cmk: number) {
    this.#w = Math.sqrt(-cmk) / (2 * spring.mass);
    this.#r = -spring.damping / (2 * spring.mass);
    this.#c1 = distance;
    this.#c2 = (velocity - this.#r * distance) / this.#w;
  }
  x(t: number): number {
    const w = this.#w;
    return Math.exp(this.#r * t) * (this.#c1 * Math.cos(w * t) + this.#c2 * Math.sin(w * t));
  }
  dx(t: number): number {
    const w = this.#w;
    const r = this.#r;
    const e = Math.exp(r * t);
    return e * ((this.#c2 * w + r * this.#c1) * Math.cos(w * t) + (r * this.#c2 - this.#c1 * w) * Math.sin(w * t));
  }
}

function makeSolution(spring: SpringDescription, distance: number, velocity: number): SpringSolution {
  const cmk = spring.damping * spring.damping - 4 * spring.mass * spring.stiffness;
  // 浮点误差内视为临界阻尼(withDampingRatio(ratio: 1) 的常见情形)。
  if (Math.abs(cmk) <= 1e-9 * Math.max(1, 4 * spring.mass * spring.stiffness)) {
    return new CriticalSolution(spring, distance, velocity);
  }
  if (cmk > 0) return new OverdampedSolution(spring, distance, velocity, cmk);
  return new UnderdampedSolution(spring, distance, velocity, cmk);
}

/** 弹簧模拟:从 start 以初速度 velocity 趋向 end。 */
export class SpringSimulation extends Simulation {
  readonly #end: number;
  readonly #solution: SpringSolution;
  constructor(spring: SpringDescription, start: number, end: number, velocity: number, tolerance?: Tolerance) {
    super(tolerance);
    this.#end = end;
    this.#solution = makeSolution(spring, start - end, velocity);
  }
  /** 阻尼类型。 */
  get type(): SpringType {
    return this.#solution.type;
  }
  x(time: number): number {
    return this.#end + this.#solution.x(time);
  }
  dx(time: number): number {
    return this.#solution.dx(time);
  }
  isDone(time: number): boolean {
    return nearZero(this.#solution.x(time), this.tolerance.distance) && nearZero(this.#solution.dx(time), this.tolerance.velocity);
  }
}

/** 摩擦减速模拟:速度按 drag^t 指数衰减(滚动惯性)。 */
export class FrictionSimulation extends Simulation {
  readonly #drag: number;
  readonly #dragLog: number;
  readonly #x: number;
  readonly #v: number;
  constructor(drag: number, position: number, velocity: number, tolerance?: Tolerance) {
    super(tolerance);
    this.#drag = drag;
    this.#dragLog = Math.log(drag);
    this.#x = position;
    this.#v = velocity;
  }
  /** 构造一个从 startPosition/startVelocity 经过 endPosition 时速度为 endVelocity 的摩擦模拟。 */
  static through(startPosition: number, endPosition: number, startVelocity: number, endVelocity: number): FrictionSimulation {
    const drag = Math.exp((startVelocity - endVelocity) / (startPosition - endPosition));
    return new FrictionSimulation(drag, startPosition, startVelocity, new Tolerance(1e-3, 1e-3, Math.abs(endVelocity)));
  }
  x(time: number): number {
    return this.#x + (this.#v * Math.pow(this.#drag, time)) / this.#dragLog - this.#v / this.#dragLog;
  }
  dx(time: number): number {
    return this.#v * Math.pow(this.#drag, time);
  }
  /** 最终停止位置。 */
  get finalX(): number {
    return this.#x - this.#v / this.#dragLog;
  }
  /** 到达位置 x 的时间;到达不了为 Infinity。 */
  timeAtX(x: number): number {
    if (x === this.#x) return 0;
    const v = this.#v;
    if (v === 0 || (v > 0 ? x < this.#x || x > this.finalX : x > this.#x || x < this.finalX)) return Infinity;
    return Math.log((this.#dragLog * (x - this.#x)) / v + 1) / this.#dragLog;
  }
  isDone(time: number): boolean {
    return Math.abs(this.dx(time)) < this.tolerance.velocity;
  }
}

/** 把另一个模拟的位置/速度钳制到区间内。 */
export class ClampedSimulation extends Simulation {
  constructor(readonly simulation: Simulation, readonly xMin = -Infinity, readonly xMax = Infinity,
              readonly dxMin = -Infinity, readonly dxMax = Infinity) {
    super(simulation.tolerance);
  }
  x(time: number): number {
    const v = this.simulation.x(time);
    return v < this.xMin ? this.xMin : v > this.xMax ? this.xMax : v;
  }
  dx(time: number): number {
    const v = this.simulation.dx(time);
    return v < this.dxMin ? this.dxMin : v > this.dxMax ? this.dxMax : v;
  }
  isDone(time: number): boolean {
    return this.simulation.isDone(time);
  }
}

/** 匀加速模拟;|x| 达到 endDistance 时结束。 */
export class GravitySimulation extends Simulation {
  constructor(readonly acceleration: number, readonly distance: number, readonly endDistance: number, readonly velocity: number) {
    super();
  }
  x(time: number): number {
    return this.distance + this.velocity * time + 0.5 * this.acceleration * time * time;
  }
  dx(time: number): number {
    return this.velocity + time * this.acceleration;
  }
  isDone(time: number): boolean {
    return Math.abs(this.x(time)) >= this.endDistance;
  }
}
