import { EdgeInsets } from "../painting/edge_insets";
import { Size, clampDouble } from "../painting/geometry";

/** 布局约束基类。 */
export abstract class Constraints {
  /** 是否只允许唯一尺寸。 */
  abstract get isTight(): boolean;
  abstract equals(other: Constraints | null): boolean;
}

/** 盒模型约束:宽高的 [min, max] 区间。 */
export class BoxConstraints extends Constraints {
  constructor(
    readonly minWidth: number = 0,
    readonly maxWidth: number = Infinity,
    readonly minHeight: number = 0,
    readonly maxHeight: number = Infinity,
  ) {
    super();
  }

  static from(o: { minWidth?: number; maxWidth?: number; minHeight?: number; maxHeight?: number }): BoxConstraints {
    return new BoxConstraints(o.minWidth ?? 0, o.maxWidth ?? Infinity, o.minHeight ?? 0, o.maxHeight ?? Infinity);
  }
  /** 只允许 size。 */
  static tight(size: Size): BoxConstraints {
    return new BoxConstraints(size.width, size.width, size.height, size.height);
  }
  /** 指定的维度紧约束, 其余不限。 */
  static tightFor(o: { width?: number | null; height?: number | null }): BoxConstraints {
    const w = o.width ?? null;
    const h = o.height ?? null;
    return new BoxConstraints(w ?? 0, w ?? Infinity, h ?? 0, h ?? Infinity);
  }
  /** 有限维度紧约束, 无限维度不限。 */
  static tightForFinite(o: { width?: number; height?: number }): BoxConstraints {
    const w = o.width ?? Infinity;
    const h = o.height ?? Infinity;
    return new BoxConstraints(w !== Infinity ? w : 0, w, h !== Infinity ? h : 0, h);
  }
  /** 0..size。 */
  static loose(size: Size): BoxConstraints {
    return new BoxConstraints(0, size.width, 0, size.height);
  }
  /** 尽量撑满(未指定维度为无穷紧约束)。 */
  static expand(o: { width?: number; height?: number } = {}): BoxConstraints {
    const w = o.width ?? Infinity;
    const h = o.height ?? Infinity;
    return new BoxConstraints(w, w, h, h);
  }

  get hasBoundedWidth(): boolean {
    return this.maxWidth < Infinity;
  }
  get hasBoundedHeight(): boolean {
    return this.maxHeight < Infinity;
  }
  get hasTightWidth(): boolean {
    return this.minWidth >= this.maxWidth;
  }
  get hasTightHeight(): boolean {
    return this.minHeight >= this.maxHeight;
  }
  get isTight(): boolean {
    return this.hasTightWidth && this.hasTightHeight;
  }
  get hasInfiniteWidth(): boolean {
    return this.minWidth >= Infinity;
  }
  get hasInfiniteHeight(): boolean {
    return this.minHeight >= Infinity;
  }
  get isNormalized(): boolean {
    return this.minWidth >= 0 && this.minWidth <= this.maxWidth && this.minHeight >= 0 && this.minHeight <= this.maxHeight;
  }
  get biggest(): Size {
    return new Size(this.constrainWidth(), this.constrainHeight());
  }
  get smallest(): Size {
    return new Size(this.constrainWidth(0), this.constrainHeight(0));
  }

  constrainWidth(width: number = Infinity): number {
    return clampDouble(width, this.minWidth, this.maxWidth);
  }
  constrainHeight(height: number = Infinity): number {
    return clampDouble(height, this.minHeight, this.maxHeight);
  }
  /** 把 size 钳制到约束内。 */
  constrain(size: Size): Size {
    const w = this.constrainWidth(size.width);
    const h = this.constrainHeight(size.height);
    return w === size.width && h === size.height ? size : new Size(w, h);
  }
  /** 保持宽高比地钳制(用于图片)。 */
  constrainSizeAndAttemptToPreserveAspectRatio(size: Size): Size {
    if (this.isTight) return this.smallest;
    let width = size.width;
    let height = size.height;
    if (width <= 0 || height <= 0) return this.constrain(size);
    const aspect = width / height;
    if (width > this.maxWidth) { width = this.maxWidth; height = width / aspect; }
    if (height > this.maxHeight) { height = this.maxHeight; width = height * aspect; }
    if (width < this.minWidth) { width = this.minWidth; height = width / aspect; }
    if (height < this.minHeight) { height = this.minHeight; width = height * aspect; }
    return new Size(this.constrainWidth(width), this.constrainHeight(height));
  }
  /** 去掉 insets。 */
  deflate(edges: EdgeInsets): BoxConstraints {
    const h = edges.horizontal;
    const v = edges.vertical;
    const minW = Math.max(0, this.minWidth - h);
    const minH = Math.max(0, this.minHeight - v);
    return new BoxConstraints(minW, Math.max(minW, this.maxWidth - h), minH, Math.max(minH, this.maxHeight - v));
  }
  /** 最小值归零。 */
  loosen(): BoxConstraints {
    return new BoxConstraints(0, this.maxWidth, 0, this.maxHeight);
  }
  /** 把本约束限制到 other 内。 */
  enforce(c: BoxConstraints): BoxConstraints {
    return new BoxConstraints(
      clampDouble(this.minWidth, c.minWidth, c.maxWidth), clampDouble(this.maxWidth, c.minWidth, c.maxWidth),
      clampDouble(this.minHeight, c.minHeight, c.maxHeight), clampDouble(this.maxHeight, c.minHeight, c.maxHeight));
  }
  /** 在允许范围内收紧到指定宽/高。 */
  tighten(o: { width?: number | null; height?: number | null }): BoxConstraints {
    const w = o.width ?? null;
    const h = o.height ?? null;
    return new BoxConstraints(
      w === null ? this.minWidth : clampDouble(w, this.minWidth, this.maxWidth),
      w === null ? this.maxWidth : clampDouble(w, this.minWidth, this.maxWidth),
      h === null ? this.minHeight : clampDouble(h, this.minHeight, this.maxHeight),
      h === null ? this.maxHeight : clampDouble(h, this.minHeight, this.maxHeight));
  }
  /** 只保留宽度约束。 */
  widthConstraints(): BoxConstraints {
    return new BoxConstraints(this.minWidth, this.maxWidth, 0, Infinity);
  }
  heightConstraints(): BoxConstraints {
    return new BoxConstraints(0, Infinity, this.minHeight, this.maxHeight);
  }
  isSatisfiedBy(size: Size): boolean {
    return this.minWidth <= size.width && size.width <= this.maxWidth && this.minHeight <= size.height && size.height <= this.maxHeight;
  }
  equals(other: Constraints | null): boolean {
    return other instanceof BoxConstraints && other.minWidth === this.minWidth && other.maxWidth === this.maxWidth &&
      other.minHeight === this.minHeight && other.maxHeight === this.maxHeight;
  }
  toString(): string {
    const f = (n: number): string => (n === Infinity ? "∞" : n.toFixed(1));
    if (this.isTight) return `BoxConstraints(tight ${f(this.minWidth)}x${f(this.minHeight)})`;
    return `BoxConstraints(${f(this.minWidth)}<=w<=${f(this.maxWidth)}, ${f(this.minHeight)}<=h<=${f(this.maxHeight)})`;
  }
}
