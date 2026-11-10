import type { Image } from "momo:ui";
import { Alignment } from "../painting/alignment";
import { paintImage, type BoxFit } from "../painting/box_fit";
import { Offset, Rect, Size } from "../painting/geometry";
import { RenderBox } from "./box";
import { BoxConstraints } from "./box_constraints";
import type { PaintingContext } from "./object";

/** 图片渲染。尺寸:指定 width/height 优先, 否则按图片逻辑尺寸(像素/scale)保持宽高比适配约束。 */
export class RenderImage extends RenderBox {
  #image: Image | null;
  #width: number | null;
  #height: number | null;
  #scale: number;
  #fit: BoxFit | null;
  #alignment: Alignment;
  #opacity: number;

  constructor(o: { image?: Image | null; width?: number | null; height?: number | null; scale?: number; fit?: BoxFit | null; alignment?: Alignment; opacity?: number } = {}) {
    super();
    this.#image = o.image ?? null;
    this.#width = o.width ?? null;
    this.#height = o.height ?? null;
    this.#scale = o.scale ?? 1;
    this.#fit = o.fit ?? null;
    this.#alignment = o.alignment ?? Alignment.center;
    this.#opacity = o.opacity ?? 1;
  }

  get image(): Image | null {
    return this.#image;
  }
  set image(v: Image | null) {
    if (v === this.#image) return;
    const sizeChanged = v === null || this.#image === null || v.width !== this.#image.width || v.height !== this.#image.height;
    this.#image = v;
    this.markNeedsPaint();
    if (sizeChanged && (this.#width === null || this.#height === null)) this.markNeedsLayout();
  }
  set width(v: number | null) {
    if (v !== this.#width) {
      this.#width = v;
      this.markNeedsLayout();
    }
  }
  set height(v: number | null) {
    if (v !== this.#height) {
      this.#height = v;
      this.markNeedsLayout();
    }
  }
  set scale(v: number) {
    if (v !== this.#scale) {
      this.#scale = v;
      this.markNeedsLayout();
    }
  }
  set fit(v: BoxFit | null) {
    if (v !== this.#fit) {
      this.#fit = v;
      this.markNeedsPaint();
    }
  }
  set alignment(v: Alignment) {
    if (!v.equals(this.#alignment)) {
      this.#alignment = v;
      this.markNeedsPaint();
    }
  }
  set opacity(v: number) {
    if (v !== this.#opacity) {
      this.#opacity = v;
      this.markNeedsPaint();
    }
  }

  #sizeFor(constraints: BoxConstraints): Size {
    const c = constraints.enforce(BoxConstraints.tightForFinite({
      ...(this.#width !== null ? { width: this.#width } : {}),
      ...(this.#height !== null ? { height: this.#height } : {}),
    }));
    if (this.#image === null) return c.smallest;
    return c.constrainSizeAndAttemptToPreserveAspectRatio(new Size(this.#image.width / this.#scale, this.#image.height / this.#scale));
  }

  protected override computeMinIntrinsicWidth(height: number): number {
    if (this.#width === null && this.#height === null) return 0;
    return this.#sizeFor(BoxConstraints.tightForFinite({ height })).width;
  }
  protected override computeMaxIntrinsicWidth(height: number): number {
    return this.#sizeFor(BoxConstraints.tightForFinite({ height })).width;
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    if (this.#width === null && this.#height === null) return 0;
    return this.#sizeFor(BoxConstraints.tightForFinite({ width })).height;
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    return this.#sizeFor(BoxConstraints.tightForFinite({ width })).height;
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    return this.#sizeFor(constraints);
  }
  override performLayout(): void {
    this.size = this.#sizeFor(this.constraints);
  }
  override hitTestSelf(): boolean {
    return true;
  }
  override paint(context: PaintingContext, offset: Offset): void {
    if (this.#image === null || this.#image.disposed) return;
    paintImage({
      canvas: context.canvas,
      rect: Rect.fromLTWH(offset.dx, offset.dy, this.size.width, this.size.height),
      image: this.#image,
      fit: this.#fit ?? "scaleDown",
      alignment: this.#alignment,
      opacity: this.#opacity,
    });
  }
}
