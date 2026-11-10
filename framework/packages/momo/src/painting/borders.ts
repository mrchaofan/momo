import { Path, type Canvas, type Paint } from "momo:ui";
import { Color, Colors } from "./color";
import { EdgeInsets } from "./edge_insets";
import { Rect, lerpDouble } from "./geometry";

/** 四角圆角半径(圆形角)。 */
export class BorderRadius {
  static readonly zero = new BorderRadius(0, 0, 0, 0);
  constructor(readonly topLeft: number, readonly topRight: number, readonly bottomRight: number, readonly bottomLeft: number) {}
  static circular(r: number): BorderRadius {
    return new BorderRadius(r, r, r, r);
  }
  static all(r: number): BorderRadius {
    return BorderRadius.circular(r);
  }
  static only(o: { topLeft?: number; topRight?: number; bottomRight?: number; bottomLeft?: number }): BorderRadius {
    return new BorderRadius(o.topLeft ?? 0, o.topRight ?? 0, o.bottomRight ?? 0, o.bottomLeft ?? 0);
  }
  static vertical(o: { top?: number; bottom?: number }): BorderRadius {
    return new BorderRadius(o.top ?? 0, o.top ?? 0, o.bottom ?? 0, o.bottom ?? 0);
  }
  get isUniform(): boolean {
    return this.topLeft === this.topRight && this.topLeft === this.bottomRight && this.topLeft === this.bottomLeft;
  }
  get isZero(): boolean {
    return this.topLeft === 0 && this.topRight === 0 && this.bottomRight === 0 && this.bottomLeft === 0;
  }
  /** 各角按 rect 尺寸约束后的半径(不超过短边一半)。 */
  clampedFor(rect: Rect): BorderRadius {
    const max = Math.max(0, Math.min(rect.width, rect.height) / 2);
    const c = (r: number): number => Math.min(Math.max(0, r), max);
    return new BorderRadius(c(this.topLeft), c(this.topRight), c(this.bottomRight), c(this.bottomLeft));
  }
  /** 收缩(用于内边框)。 */
  deflate(d: number): BorderRadius {
    const c = (r: number): number => Math.max(0, r - d);
    return new BorderRadius(c(this.topLeft), c(this.topRight), c(this.bottomRight), c(this.bottomLeft));
  }
  equals(o: BorderRadius | null | undefined): boolean {
    return o != null && o.topLeft === this.topLeft && o.topRight === this.topRight && o.bottomRight === this.bottomRight && o.bottomLeft === this.bottomLeft;
  }
  static lerp(a: BorderRadius | null, b: BorderRadius | null, t: number): BorderRadius | null {
    if (a === null && b === null) return null;
    const x = a ?? BorderRadius.zero;
    const y = b ?? BorderRadius.zero;
    return new BorderRadius(lerpDouble(x.topLeft, y.topLeft, t), lerpDouble(x.topRight, y.topRight, t),
      lerpDouble(x.bottomRight, y.bottomRight, t), lerpDouble(x.bottomLeft, y.bottomLeft, t));
  }
}

/** 圆角矩形 = 矩形 + 圆角。 */
export class RRect {
  constructor(readonly rect: Rect, readonly radius: BorderRadius) {}
  static fromRectAndRadius(rect: Rect, r: number): RRect {
    return new RRect(rect, BorderRadius.circular(r));
  }
  get left(): number {
    return this.rect.left;
  }
  get top(): number {
    return this.rect.top;
  }
  get width(): number {
    return this.rect.width;
  }
  get height(): number {
    return this.rect.height;
  }
  shift(dx: number, dy: number): RRect {
    return new RRect(this.rect.translate(dx, dy), this.radius);
  }
  /** 点是否在圆角矩形内。 */
  contains(x: number, y: number): boolean {
    const r = this.rect;
    if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) return false;
    const rad = this.radius.clampedFor(r);
    const corner = (cx: number, cy: number, rr: number): boolean => {
      const dx = x - cx;
      const dy = y - cy;
      return dx * dx + dy * dy <= rr * rr;
    };
    if (x < r.left + rad.topLeft && y < r.top + rad.topLeft) return corner(r.left + rad.topLeft, r.top + rad.topLeft, rad.topLeft);
    if (x > r.right - rad.topRight && y < r.top + rad.topRight) return corner(r.right - rad.topRight, r.top + rad.topRight, rad.topRight);
    if (x > r.right - rad.bottomRight && y > r.bottom - rad.bottomRight) return corner(r.right - rad.bottomRight, r.bottom - rad.bottomRight, rad.bottomRight);
    if (x < r.left + rad.bottomLeft && y > r.bottom - rad.bottomLeft) return corner(r.left + rad.bottomLeft, r.bottom - rad.bottomLeft, rad.bottomLeft);
    return true;
  }
}

const KAPPA = 0.5522847498;

/** 构造(可非均匀)圆角矩形路径。 */
export function rrectPath(rect: Rect, radius: BorderRadius, path: Path = new Path()): Path {
  const r = radius.clampedFor(rect);
  const { left: l, top: t, right: rr, bottom: b } = rect;
  path.moveTo(l + r.topLeft, t);
  path.lineTo(rr - r.topRight, t);
  if (r.topRight > 0) path.cubicTo(rr - r.topRight + r.topRight * KAPPA, t, rr, t + r.topRight - r.topRight * KAPPA, rr, t + r.topRight);
  path.lineTo(rr, b - r.bottomRight);
  if (r.bottomRight > 0) path.cubicTo(rr, b - r.bottomRight + r.bottomRight * KAPPA, rr - r.bottomRight + r.bottomRight * KAPPA, b, rr - r.bottomRight, b);
  path.lineTo(l + r.bottomLeft, b);
  if (r.bottomLeft > 0) path.cubicTo(l + r.bottomLeft - r.bottomLeft * KAPPA, b, l, b - r.bottomLeft + r.bottomLeft * KAPPA, l, b - r.bottomLeft);
  path.lineTo(l, t + r.topLeft);
  if (r.topLeft > 0) path.cubicTo(l, t + r.topLeft - r.topLeft * KAPPA, l + r.topLeft - r.topLeft * KAPPA, t, l + r.topLeft, t);
  return path.close();
}

/** 画圆角矩形:均匀圆角直接 drawRRect, 否则走路径。 */
export function drawRRect(canvas: Canvas, rect: Rect, radius: BorderRadius, paint: Paint): void {
  if (radius.isZero) canvas.drawRect(rect.left, rect.top, rect.width, rect.height, paint);
  else if (radius.isUniform) canvas.drawRRect(rect.left, rect.top, rect.width, rect.height, radius.clampedFor(rect).topLeft, paint);
  else canvas.drawPath(rrectPath(rect, radius), paint);
}

/** 按圆角矩形裁剪。 */
export function clipRRect(canvas: Canvas, rect: Rect, radius: BorderRadius): void {
  if (radius.isZero) canvas.clipRect(rect.left, rect.top, rect.width, rect.height);
  else if (radius.isUniform) canvas.clipRRect(rect.left, rect.top, rect.width, rect.height, radius.clampedFor(rect).topLeft);
  else canvas.clipPath(rrectPath(rect, radius));
}

/** 边框线样式。 */
export type BorderStyle = "none" | "solid";

/** 一条边。 */
export class BorderSide {
  static readonly none = new BorderSide({ width: 0, style: "none" });
  readonly color: Color;
  readonly width: number;
  readonly style: BorderStyle;
  constructor(o: { color?: Color; width?: number; style?: BorderStyle } = {}) {
    this.color = o.color ?? Colors.black;
    this.width = o.width ?? 1;
    this.style = o.style ?? "solid";
  }
  get effectiveWidth(): number {
    return this.style === "none" ? 0 : this.width;
  }
  equals(o: BorderSide): boolean {
    return o.color.equals(this.color) && o.width === this.width && o.style === this.style;
  }
  copyWith(o: { color?: Color; width?: number; style?: BorderStyle }): BorderSide {
    return new BorderSide({ color: o.color ?? this.color, width: o.width ?? this.width, style: o.style ?? this.style });
  }
}

/** 盒子四边边框。 */
export class Border {
  readonly top: BorderSide;
  readonly right: BorderSide;
  readonly bottom: BorderSide;
  readonly left: BorderSide;
  constructor(o: { top?: BorderSide; right?: BorderSide; bottom?: BorderSide; left?: BorderSide } = {}) {
    this.top = o.top ?? BorderSide.none;
    this.right = o.right ?? BorderSide.none;
    this.bottom = o.bottom ?? BorderSide.none;
    this.left = o.left ?? BorderSide.none;
  }
  static all(o: { color?: Color; width?: number; style?: BorderStyle } = {}): Border {
    const s = new BorderSide(o);
    return new Border({ top: s, right: s, bottom: s, left: s });
  }
  static fromSide(s: BorderSide): Border {
    return new Border({ top: s, right: s, bottom: s, left: s });
  }
  get isUniform(): boolean {
    return this.top.equals(this.right) && this.top.equals(this.bottom) && this.top.equals(this.left);
  }
  /** 边框占用的内边距。 */
  get dimensions(): EdgeInsets {
    return new EdgeInsets(this.left.effectiveWidth, this.top.effectiveWidth, this.right.effectiveWidth, this.bottom.effectiveWidth);
  }

  /** 绘制边框(内描边)。 */
  paint(canvas: Canvas, rect: Rect, paint: Paint, o: { borderRadius?: BorderRadius | null; circle?: boolean } = {}): void {
    if (this.isUniform) {
      const side = this.top;
      if (side.style === "none" || side.width <= 0) return;
      paint.color = side.color.value;
      paint.style = "fill";
      const w = side.width;
      if (o.circle) {
        paint.style = "stroke";
        paint.strokeWidth = w;
        const r = Math.min(rect.width, rect.height) / 2 - w / 2;
        canvas.drawCircle(rect.center.dx, rect.center.dy, Math.max(0, r), paint);
        paint.style = "fill";
        return;
      }
      const inner = rect.deflate(w);
      const br = o.borderRadius ?? BorderRadius.zero;
      if (br.isUniform) {
        const outerR = br.clampedFor(rect).topLeft;
        canvas.drawDRRect(rect.left, rect.top, rect.width, rect.height, outerR,
          inner.left, inner.top, Math.max(0, inner.width), Math.max(0, inner.height), Math.max(0, outerR - w), paint);
      } else {
        paint.style = "stroke";
        paint.strokeWidth = w;
        canvas.drawPath(rrectPath(rect.deflate(w / 2), br.deflate(w / 2)), paint);
        paint.style = "fill";
      }
      return;
    }
    // 非均匀:逐边画矩形(忽略圆角)。
    const sides: [BorderSide, number, number, number, number][] = [
      [this.top, rect.left, rect.top, rect.width, this.top.effectiveWidth],
      [this.bottom, rect.left, rect.bottom - this.bottom.effectiveWidth, rect.width, this.bottom.effectiveWidth],
      [this.left, rect.left, rect.top, this.left.effectiveWidth, rect.height],
      [this.right, rect.right - this.right.effectiveWidth, rect.top, this.right.effectiveWidth, rect.height],
    ];
    paint.style = "fill";
    for (const [side, x, y, w, h] of sides) {
      if (side.effectiveWidth <= 0) continue;
      paint.color = side.color.value;
      canvas.drawRect(x, y, w, h, paint);
    }
  }
}
