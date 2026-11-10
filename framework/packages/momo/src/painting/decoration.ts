import { Gradient as UiGradient, Paint, type Canvas, type TileMode } from "momo:ui";
import { Alignment } from "./alignment";
import { Border, BorderRadius, RRect, drawRRect } from "./borders";
import { Color, Colors } from "./color";
import { EdgeInsets } from "./edge_insets";
import { Offset, Rect, Size } from "./geometry";

/** 模糊半径 → 高斯 sigma(与 Flutter 相同的换算)。 */
export function convertRadiusToSigma(radius: number): number {
  return radius > 0 ? radius * 0.57735 + 0.5 : 0;
}

/** 盒子阴影。 */
export class BoxShadow {
  readonly color: Color;
  readonly offset: Offset;
  readonly blurRadius: number;
  readonly spreadRadius: number;
  constructor(o: { color?: Color; offset?: Offset; blurRadius?: number; spreadRadius?: number } = {}) {
    this.color = o.color ?? new Color(0xff000000);
    this.offset = o.offset ?? Offset.zero;
    this.blurRadius = o.blurRadius ?? 0;
    this.spreadRadius = o.spreadRadius ?? 0;
  }
  get blurSigma(): number {
    return convertRadiusToSigma(this.blurRadius);
  }
}

/** 渐变基类。 */
export abstract class Gradient {
  constructor(readonly colors: readonly Color[], readonly stops: readonly number[] | null, readonly tileMode: TileMode) {}
  /** 为给定矩形创建着色器。 */
  abstract createShader(rect: Rect): UiGradient;
  protected colorValues(): number[] {
    return this.colors.map((c) => c.value);
  }
}

/** 线性渐变(begin/end 为相对矩形的对齐点)。 */
export class LinearGradient extends Gradient {
  readonly begin: Alignment;
  readonly end: Alignment;
  constructor(o: { colors: readonly Color[]; begin?: Alignment; end?: Alignment; stops?: readonly number[]; tileMode?: TileMode }) {
    super(o.colors, o.stops ?? null, o.tileMode ?? "clamp");
    this.begin = o.begin ?? Alignment.centerLeft;
    this.end = o.end ?? Alignment.centerRight;
  }
  createShader(rect: Rect): UiGradient {
    const a = this.begin.withinRect(rect);
    const b = this.end.withinRect(rect);
    return UiGradient.linear(a.dx, a.dy, b.dx, b.dy, this.colorValues(), this.stops ? [...this.stops] : null, this.tileMode);
  }
}

/** 径向渐变(radius 相对矩形短边)。 */
export class RadialGradient extends Gradient {
  readonly center: Alignment;
  readonly radius: number;
  constructor(o: { colors: readonly Color[]; center?: Alignment; radius?: number; stops?: readonly number[]; tileMode?: TileMode }) {
    super(o.colors, o.stops ?? null, o.tileMode ?? "clamp");
    this.center = o.center ?? Alignment.center;
    this.radius = o.radius ?? 0.5;
  }
  createShader(rect: Rect): UiGradient {
    const c = this.center.withinRect(rect);
    return UiGradient.radial(c.dx, c.dy, this.radius * Math.min(rect.width, rect.height), this.colorValues(),
      this.stops ? [...this.stops] : null, this.tileMode);
  }
}

/** 盒子形状。 */
export type BoxShape = "rectangle" | "circle";

/** 装饰抽象:绘制背景/前景。 */
export abstract class Decoration {
  /** 装饰占用的内边距(如边框宽度)。 */
  get padding(): EdgeInsets {
    return EdgeInsets.zero;
  }
  /** 点是否落在装饰形状内(局部坐标)。 */
  hitTest(size: Size, position: Offset): boolean {
    return position.dx >= 0 && position.dy >= 0 && position.dx < size.width && position.dy < size.height;
  }
  /** 在 rect 内绘制。 */
  abstract paint(canvas: Canvas, rect: Rect): void;
}

const scratch = new Paint();
function resetPaint(p: Paint): Paint {
  p.color = 0xff000000;
  p.style = "fill";
  p.strokeWidth = 0;
  p.shader = null;
  p.maskBlurSigma = 0;
  p.antiAlias = true;
  return p;
}

/** 盒子装饰:颜色/渐变/圆角/边框/阴影/形状(对标 Flutter BoxDecoration)。 */
export class BoxDecoration extends Decoration {
  readonly color: Color | null;
  readonly gradient: Gradient | null;
  readonly border: Border | null;
  readonly borderRadius: BorderRadius | null;
  readonly boxShadow: readonly BoxShadow[] | null;
  readonly shape: BoxShape;

  constructor(o: {
    color?: Color | null;
    gradient?: Gradient | null;
    border?: Border | null;
    borderRadius?: BorderRadius | null;
    boxShadow?: readonly BoxShadow[] | null;
    shape?: BoxShape;
  } = {}) {
    super();
    this.color = o.color ?? null;
    this.gradient = o.gradient ?? null;
    this.border = o.border ?? null;
    this.borderRadius = o.borderRadius ?? null;
    this.boxShadow = o.boxShadow ?? null;
    this.shape = o.shape ?? "rectangle";
  }

  copyWith(o: { color?: Color | null; gradient?: Gradient | null; border?: Border | null; borderRadius?: BorderRadius | null;
    boxShadow?: readonly BoxShadow[] | null; shape?: BoxShape }): BoxDecoration {
    return new BoxDecoration({
      color: o.color !== undefined ? o.color : this.color,
      gradient: o.gradient !== undefined ? o.gradient : this.gradient,
      border: o.border !== undefined ? o.border : this.border,
      borderRadius: o.borderRadius !== undefined ? o.borderRadius : this.borderRadius,
      boxShadow: o.boxShadow !== undefined ? o.boxShadow : this.boxShadow,
      shape: o.shape ?? this.shape,
    });
  }

  override get padding(): EdgeInsets {
    return this.border?.dimensions ?? EdgeInsets.zero;
  }

  override hitTest(size: Size, position: Offset): boolean {
    if (this.shape === "circle") {
      const c = size.center(Offset.zero);
      const r = size.shortestSide / 2;
      return position.minus(c).distanceSquared <= r * r;
    }
    if (this.borderRadius && !this.borderRadius.isZero) {
      return new RRect(Rect.fromLTWH(0, 0, size.width, size.height), this.borderRadius).contains(position.dx, position.dy);
    }
    return super.hitTest(size, position);
  }

  paint(canvas: Canvas, rect: Rect): void {
    const p = resetPaint(scratch);
    const br = this.borderRadius ?? BorderRadius.zero;
    const circle = this.shape === "circle";
    if (this.boxShadow) {
      for (const s of this.boxShadow) {
        resetPaint(p);
        p.color = s.color.value;
        p.maskBlurSigma = s.blurSigma;
        const r = rect.shift(s.offset).inflate(s.spreadRadius);
        if (circle) canvas.drawCircle(r.center.dx, r.center.dy, Math.min(r.width, r.height) / 2, p);
        else drawRRect(canvas, r, br, p);
      }
      resetPaint(p);
    }
    if (this.color !== null || this.gradient !== null) {
      p.color = this.color?.value ?? Colors.black.value;
      p.shader = this.gradient ? this.gradient.createShader(rect) : null;
      if (circle) canvas.drawCircle(rect.center.dx, rect.center.dy, Math.min(rect.width, rect.height) / 2, p);
      else drawRRect(canvas, rect, br, p);
      p.shader = null;
    }
    if (this.border) {
      resetPaint(p);
      this.border.paint(canvas, rect, p, { borderRadius: this.borderRadius, circle });
    }
  }
}
