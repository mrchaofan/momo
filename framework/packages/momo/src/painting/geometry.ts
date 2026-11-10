/** 线性插值。 */
export function lerpDouble(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

/** 限制到 [min, max]。 */
export function clampDouble(x: number, min: number, max: number): number {
  return x < min ? min : x > max ? max : x;
}

/** 二维偏移/点(不可变)。 */
export class Offset {
  static readonly zero = new Offset(0, 0);
  static readonly infinite = new Offset(Infinity, Infinity);
  constructor(readonly dx: number, readonly dy: number) {}
  get distance(): number {
    return Math.sqrt(this.dx * this.dx + this.dy * this.dy);
  }
  get distanceSquared(): number {
    return this.dx * this.dx + this.dy * this.dy;
  }
  get isFinite(): boolean {
    return Number.isFinite(this.dx) && Number.isFinite(this.dy);
  }
  plus(o: Offset): Offset {
    return new Offset(this.dx + o.dx, this.dy + o.dy);
  }
  minus(o: Offset): Offset {
    return new Offset(this.dx - o.dx, this.dy - o.dy);
  }
  translate(dx: number, dy: number): Offset {
    return new Offset(this.dx + dx, this.dy + dy);
  }
  scale(sx: number, sy: number = sx): Offset {
    return new Offset(this.dx * sx, this.dy * sy);
  }
  neg(): Offset {
    return new Offset(-this.dx, -this.dy);
  }
  equals(o: Offset | null | undefined): boolean {
    return o != null && o.dx === this.dx && o.dy === this.dy;
  }
  static lerp(a: Offset, b: Offset, t: number): Offset {
    return new Offset(lerpDouble(a.dx, b.dx, t), lerpDouble(a.dy, b.dy, t));
  }
  toString(): string {
    return `Offset(${this.dx.toFixed(1)}, ${this.dy.toFixed(1)})`;
  }
}

/** 二维尺寸(不可变)。 */
export class Size {
  static readonly zero = new Size(0, 0);
  static readonly infinite = new Size(Infinity, Infinity);
  constructor(readonly width: number, readonly height: number) {}
  static square(d: number): Size {
    return new Size(d, d);
  }
  get isEmpty(): boolean {
    return this.width <= 0 || this.height <= 0;
  }
  get shortestSide(): number {
    return Math.min(Math.abs(this.width), Math.abs(this.height));
  }
  get longestSide(): number {
    return Math.max(Math.abs(this.width), Math.abs(this.height));
  }
  get aspectRatio(): number {
    return this.height !== 0 ? this.width / this.height : 0;
  }
  /** 点是否在 [0,w)×[0,h) 内。 */
  contains(o: Offset): boolean {
    return o.dx >= 0 && o.dx < this.width && o.dy >= 0 && o.dy < this.height;
  }
  center(origin: Offset): Offset {
    return new Offset(origin.dx + this.width / 2, origin.dy + this.height / 2);
  }
  equals(o: Size | null | undefined): boolean {
    return o != null && o.width === this.width && o.height === this.height;
  }
  static lerp(a: Size, b: Size, t: number): Size {
    return new Size(lerpDouble(a.width, b.width, t), lerpDouble(a.height, b.height, t));
  }
  toString(): string {
    return `Size(${this.width.toFixed(1)}, ${this.height.toFixed(1)})`;
  }
}

/** 轴对齐矩形(不可变, LTRB)。 */
export class Rect {
  static readonly zero = new Rect(0, 0, 0, 0);
  static readonly largest = new Rect(-1e9, -1e9, 1e9, 1e9);
  constructor(readonly left: number, readonly top: number, readonly right: number, readonly bottom: number) {}
  static fromLTWH(left: number, top: number, width: number, height: number): Rect {
    return new Rect(left, top, left + width, top + height);
  }
  static fromLTRB(left: number, top: number, right: number, bottom: number): Rect {
    return new Rect(left, top, right, bottom);
  }
  static fromCenter(center: Offset, width: number, height: number): Rect {
    return new Rect(center.dx - width / 2, center.dy - height / 2, center.dx + width / 2, center.dy + height / 2);
  }
  static fromPoints(a: Offset, b: Offset): Rect {
    return new Rect(Math.min(a.dx, b.dx), Math.min(a.dy, b.dy), Math.max(a.dx, b.dx), Math.max(a.dy, b.dy));
  }
  static fromOffsetSize(o: Offset, s: Size): Rect {
    return new Rect(o.dx, o.dy, o.dx + s.width, o.dy + s.height);
  }
  get width(): number {
    return this.right - this.left;
  }
  get height(): number {
    return this.bottom - this.top;
  }
  get size(): Size {
    return new Size(this.width, this.height);
  }
  get topLeft(): Offset {
    return new Offset(this.left, this.top);
  }
  get bottomRight(): Offset {
    return new Offset(this.right, this.bottom);
  }
  get center(): Offset {
    return new Offset((this.left + this.right) / 2, (this.top + this.bottom) / 2);
  }
  get isEmpty(): boolean {
    return this.left >= this.right || this.top >= this.bottom;
  }
  shift(o: Offset): Rect {
    return new Rect(this.left + o.dx, this.top + o.dy, this.right + o.dx, this.bottom + o.dy);
  }
  translate(dx: number, dy: number): Rect {
    return new Rect(this.left + dx, this.top + dy, this.right + dx, this.bottom + dy);
  }
  inflate(d: number): Rect {
    return new Rect(this.left - d, this.top - d, this.right + d, this.bottom + d);
  }
  deflate(d: number): Rect {
    return this.inflate(-d);
  }
  intersect(o: Rect): Rect {
    return new Rect(Math.max(this.left, o.left), Math.max(this.top, o.top), Math.min(this.right, o.right), Math.min(this.bottom, o.bottom));
  }
  expandToInclude(o: Rect): Rect {
    return new Rect(Math.min(this.left, o.left), Math.min(this.top, o.top), Math.max(this.right, o.right), Math.max(this.bottom, o.bottom));
  }
  overlaps(o: Rect): boolean {
    return this.right > o.left && o.right > this.left && this.bottom > o.top && o.bottom > this.top;
  }
  /** 点是否在 [left,right)×[top,bottom) 内。 */
  contains(o: Offset): boolean {
    return o.dx >= this.left && o.dx < this.right && o.dy >= this.top && o.dy < this.bottom;
  }
  equals(o: Rect | null | undefined): boolean {
    return o != null && o.left === this.left && o.top === this.top && o.right === this.right && o.bottom === this.bottom;
  }
  static lerp(a: Rect, b: Rect, t: number): Rect {
    return new Rect(lerpDouble(a.left, b.left, t), lerpDouble(a.top, b.top, t), lerpDouble(a.right, b.right, t), lerpDouble(a.bottom, b.bottom, t));
  }
  toString(): string {
    return `Rect.fromLTRB(${this.left.toFixed(1)}, ${this.top.toFixed(1)}, ${this.right.toFixed(1)}, ${this.bottom.toFixed(1)})`;
  }
}

/** 主轴方向。 */
export type Axis = "horizontal" | "vertical";

/** 另一个轴。 */
export function flipAxis(axis: Axis): Axis {
  return axis === "horizontal" ? "vertical" : "horizontal";
}

/** 文本/布局方向。 */
export type TextDirection = "ltr" | "rtl";

/** 垂直方向(Flex 主轴为纵向时的摆放顺序)。 */
export type VerticalDirection = "down" | "up";

/** 裁剪行为。 */
export type Clip = "none" | "hardEdge" | "antiAlias";
