import { Offset, Rect, Size, lerpDouble } from "./geometry";

/** 四边内/外边距(逻辑像素)。 */
export class EdgeInsets {
  static readonly zero = new EdgeInsets(0, 0, 0, 0);
  constructor(readonly left: number, readonly top: number, readonly right: number, readonly bottom: number) {}
  static all(v: number): EdgeInsets {
    return new EdgeInsets(v, v, v, v);
  }
  static fromLTRB(l: number, t: number, r: number, b: number): EdgeInsets {
    return new EdgeInsets(l, t, r, b);
  }
  static symmetric(o: { horizontal?: number; vertical?: number }): EdgeInsets {
    const h = o.horizontal ?? 0;
    const v = o.vertical ?? 0;
    return new EdgeInsets(h, v, h, v);
  }
  static only(o: { left?: number; top?: number; right?: number; bottom?: number }): EdgeInsets {
    return new EdgeInsets(o.left ?? 0, o.top ?? 0, o.right ?? 0, o.bottom ?? 0);
  }
  /** 左右之和。 */
  get horizontal(): number {
    return this.left + this.right;
  }
  /** 上下之和。 */
  get vertical(): number {
    return this.top + this.bottom;
  }
  get topLeft(): Offset {
    return new Offset(this.left, this.top);
  }
  get collapsedSize(): Size {
    return new Size(this.horizontal, this.vertical);
  }
  get isNonNegative(): boolean {
    return this.left >= 0 && this.top >= 0 && this.right >= 0 && this.bottom >= 0;
  }
  add(o: EdgeInsets): EdgeInsets {
    return new EdgeInsets(this.left + o.left, this.top + o.top, this.right + o.right, this.bottom + o.bottom);
  }
  subtract(o: EdgeInsets): EdgeInsets {
    return new EdgeInsets(this.left - o.left, this.top - o.top, this.right - o.right, this.bottom - o.bottom);
  }
  scale(f: number): EdgeInsets {
    return new EdgeInsets(this.left * f, this.top * f, this.right * f, this.bottom * f);
  }
  copyWith(o: { left?: number; top?: number; right?: number; bottom?: number }): EdgeInsets {
    return new EdgeInsets(o.left ?? this.left, o.top ?? this.top, o.right ?? this.right, o.bottom ?? this.bottom);
  }
  /** 矩形向内收缩。 */
  deflateRect(r: Rect): Rect {
    return new Rect(r.left + this.left, r.top + this.top, r.right - this.right, r.bottom - this.bottom);
  }
  inflateRect(r: Rect): Rect {
    return new Rect(r.left - this.left, r.top - this.top, r.right + this.right, r.bottom + this.bottom);
  }
  deflateSize(s: Size): Size {
    return new Size(s.width - this.horizontal, s.height - this.vertical);
  }
  inflateSize(s: Size): Size {
    return new Size(s.width + this.horizontal, s.height + this.vertical);
  }
  equals(o: EdgeInsets | null | undefined): boolean {
    return o != null && o.left === this.left && o.top === this.top && o.right === this.right && o.bottom === this.bottom;
  }
  static lerp(a: EdgeInsets | null, b: EdgeInsets | null, t: number): EdgeInsets | null {
    if (a === null && b === null) return null;
    const x = a ?? EdgeInsets.zero;
    const y = b ?? EdgeInsets.zero;
    return new EdgeInsets(lerpDouble(x.left, y.left, t), lerpDouble(x.top, y.top, t), lerpDouble(x.right, y.right, t), lerpDouble(x.bottom, y.bottom, t));
  }
  toString(): string {
    return `EdgeInsets(${this.left}, ${this.top}, ${this.right}, ${this.bottom})`;
  }
}
