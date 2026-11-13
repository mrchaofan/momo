import type { Canvas } from "momo:ui";
import type { Listenable, VoidCallback } from "../foundation/change_notifier";
import {
  PointerCancelEvent, PointerDownEvent, PointerEnterEvent, PointerExitEvent, PointerHoverEvent, PointerMoveEvent,
  PointerSignalEvent, PointerUpEvent, type PointerEvent,
} from "../gestures/events";
import type { HitTestEntry, HitTestResult } from "../gestures/hit_test";
import { Alignment } from "../painting/alignment";
import { BorderRadius, RRect } from "../painting/borders";
import type { Decoration } from "../painting/decoration";
import { Offset, Rect, Size, type Clip } from "../painting/geometry";
import { Matrix4 } from "../painting/matrix4";
import {
  RenderBox, RenderProxyBox, RenderProxyBoxWithHitTestBehavior, type HitTestBehavior,
} from "./box";
import { BoxConstraints } from "./box_constraints";
import { ClipRRectLayer, ClipRectLayer, OpacityLayer, TextureLayer, TransformLayer } from "./layer";
import { kMouseTrackerAnnotation, type MouseCursor, type MouseTrackerAnnotation } from "./mouse_tracker";
import type { PaintingContext, PipelineOwner, RenderObject } from "./object";

/** 在约束上叠加额外约束。 */
export class RenderConstrainedBox extends RenderProxyBox {
  #additional: BoxConstraints;
  constructor(additionalConstraints: BoxConstraints, child: RenderBox | null = null) {
    super(child);
    this.#additional = additionalConstraints;
  }
  get additionalConstraints(): BoxConstraints {
    return this.#additional;
  }
  set additionalConstraints(v: BoxConstraints) {
    if (v.equals(this.#additional)) return;
    this.#additional = v;
    this.markNeedsLayout();
  }
  protected override computeMinIntrinsicWidth(height: number): number {
    const a = this.#additional;
    if (a.hasBoundedWidth && a.hasTightWidth) return a.minWidth;
    const w = super.computeMinIntrinsicWidth(height);
    return a.hasInfiniteWidth ? w : a.constrainWidth(w);
  }
  protected override computeMaxIntrinsicWidth(height: number): number {
    const a = this.#additional;
    if (a.hasBoundedWidth && a.hasTightWidth) return a.minWidth;
    const w = super.computeMaxIntrinsicWidth(height);
    return a.hasInfiniteWidth ? w : a.constrainWidth(w);
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    const a = this.#additional;
    if (a.hasBoundedHeight && a.hasTightHeight) return a.minHeight;
    const h = super.computeMinIntrinsicHeight(width);
    return a.hasInfiniteHeight ? h : a.constrainHeight(h);
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    const a = this.#additional;
    if (a.hasBoundedHeight && a.hasTightHeight) return a.minHeight;
    const h = super.computeMaxIntrinsicHeight(width);
    return a.hasInfiniteHeight ? h : a.constrainHeight(h);
  }
  override performLayout(): void {
    const c = this.#additional.enforce(this.constraints);
    const child = this.child;
    if (child !== null) {
      child.layout(c, true);
      this.size = child.size;
    } else {
      this.size = c.constrain(Size.zero);
    }
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    const c = this.#additional.enforce(constraints);
    return this.child !== null ? this.child.getDryLayout(c) : c.constrain(Size.zero);
  }
  override debugDescribe(): string {
    return `${super.debugDescribe()} additional=${this.#additional}`;
  }
}

/** 约束无界时限制最大尺寸。 */
export class RenderLimitedBox extends RenderProxyBox {
  #maxWidth: number;
  #maxHeight: number;
  constructor(o: { maxWidth?: number; maxHeight?: number } = {}, child: RenderBox | null = null) {
    super(child);
    this.#maxWidth = o.maxWidth ?? Infinity;
    this.#maxHeight = o.maxHeight ?? Infinity;
  }
  set maxWidth(v: number) {
    if (v !== this.#maxWidth) {
      this.#maxWidth = v;
      this.markNeedsLayout();
    }
  }
  set maxHeight(v: number) {
    if (v !== this.#maxHeight) {
      this.#maxHeight = v;
      this.markNeedsLayout();
    }
  }
  #limit(c: BoxConstraints): BoxConstraints {
    return new BoxConstraints(c.minWidth, c.hasBoundedWidth ? c.maxWidth : c.constrainWidth(this.#maxWidth),
      c.minHeight, c.hasBoundedHeight ? c.maxHeight : c.constrainHeight(this.#maxHeight));
  }
  override performLayout(): void {
    const c = this.#limit(this.constraints);
    if (this.child !== null) {
      this.child.layout(c, true);
      this.size = this.constraints.constrain(this.child.size);
    } else {
      this.size = c.constrain(Size.zero);
    }
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    const c = this.#limit(constraints);
    return this.child !== null ? constraints.constrain(this.child.getDryLayout(c)) : c.constrain(Size.zero);
  }
}

/** 固定宽高比。 */
export class RenderAspectRatio extends RenderProxyBox {
  #ratio: number;
  constructor(aspectRatio: number, child: RenderBox | null = null) {
    super(child);
    this.#ratio = aspectRatio;
  }
  set aspectRatio(v: number) {
    if (v !== this.#ratio) {
      this.#ratio = v;
      this.markNeedsLayout();
    }
  }
  #apply(c: BoxConstraints): Size {
    if (c.isTight) return c.smallest;
    let width = c.maxWidth;
    let height: number;
    if (Number.isFinite(width)) height = width / this.#ratio;
    else {
      height = c.maxHeight;
      width = height * this.#ratio;
    }
    if (width > c.maxWidth) { width = c.maxWidth; height = width / this.#ratio; }
    if (height > c.maxHeight) { height = c.maxHeight; width = height * this.#ratio; }
    if (width < c.minWidth) { width = c.minWidth; height = width / this.#ratio; }
    if (height < c.minHeight) { height = c.minHeight; width = height * this.#ratio; }
    return c.constrain(new Size(width, height));
  }
  override performLayout(): void {
    this.size = this.#apply(this.constraints);
    this.child?.layout(BoxConstraints.tight(this.size));
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    return this.#apply(constraints);
  }
}

/** 整体透明度(0..1)。部分透明时使用 OpacityLayer。 */
export class RenderOpacity extends RenderProxyBox {
  #opacity: number;
  #alpha: number;
  #layer: OpacityLayer | null = null;
  constructor(opacity = 1, child: RenderBox | null = null) {
    super(child);
    this.#opacity = opacity;
    this.#alpha = Math.round(opacity * 255);
  }
  get opacity(): number {
    return this.#opacity;
  }
  set opacity(v: number) {
    if (v === this.#opacity) return;
    const wasComposited = this.alwaysNeedsCompositing;
    const wasVisible = this.#alpha !== 0;
    this.#opacity = v;
    this.#alpha = Math.round(v * 255);
    if (wasComposited !== this.alwaysNeedsCompositing) this.markNeedsCompositingBitsUpdate();
    this.markNeedsPaint();
    if (wasVisible !== (this.#alpha !== 0)) this.markNeedsPaint();
  }
  override get alwaysNeedsCompositing(): boolean {
    return this.child !== null && this.#alpha > 0 && this.#alpha < 255;
  }
  override paint(context: PaintingContext, offset: Offset): void {
    const child = this.child;
    if (child === null || this.#alpha === 0) {
      this.#layer = null;
      return;
    }
    if (this.#alpha === 255) {
      this.#layer = null;
      context.paintChild(child, offset);
      return;
    }
    this.#layer = context.pushOpacity(offset, this.#alpha, (ctx, o) => ctx.paintChild(child, o), this.#layer);
  }
}

/** 变换(可指定原点/对齐点)。 */
export class RenderTransform extends RenderProxyBox {
  #transform: Matrix4;
  #origin: Offset | null;
  #alignment: Alignment | null;
  transformHitTests: boolean;
  #layer: TransformLayer | null = null;
  constructor(o: { transform: Matrix4; origin?: Offset | null; alignment?: Alignment | null; transformHitTests?: boolean }, child: RenderBox | null = null) {
    super(child);
    this.#transform = o.transform.clone();
    this.#origin = o.origin ?? null;
    this.#alignment = o.alignment ?? null;
    this.transformHitTests = o.transformHitTests ?? true;
  }
  set transform(v: Matrix4) {
    if (v.equals(this.#transform)) return;
    this.#transform = v.clone();
    this.markNeedsPaint();
  }
  set origin(v: Offset | null) {
    this.#origin = v;
    this.markNeedsPaint();
  }
  set alignment(v: Alignment | null) {
    this.#alignment = v;
    this.markNeedsPaint();
  }
  /** 计入原点与对齐后的实际变换。 */
  get effectiveTransform(): Matrix4 {
    const o = this.#origin;
    const a = this.#alignment;
    if (o === null && a === null) return this.#transform;
    let tx = 0;
    let ty = 0;
    if (o !== null) {
      tx += o.dx;
      ty += o.dy;
    }
    if (a !== null) {
      const p = a.alongSize(this.size);
      tx += p.dx;
      ty += p.dy;
    }
    return Matrix4.translationValues(tx, ty).multiply(this.#transform).translate(-tx, -ty);
  }
  override hitTest(result: HitTestResult, position: Offset): boolean {
    return this.hitTestChildren(result, position);
  }
  override hitTestChildren(result: HitTestResult, position: Offset): boolean {
    const child = this.child;
    if (child === null) return false;
    if (!this.transformHitTests) return child.hitTest(result, position);
    return result.addWithPaintTransform(this.effectiveTransform, position, (r, p) => child.hitTest(r, p));
  }
  override applyPaintTransform(_child: RenderBox, transform: Matrix4): void {
    transform.multiply(this.effectiveTransform);
  }
  override paint(context: PaintingContext, offset: Offset): void {
    const child = this.child;
    if (child === null) return;
    const t = this.effectiveTransform;
    const translation = t.getAsTranslation();
    if (translation !== null) {
      this.#layer = null;
      context.paintChild(child, offset.plus(translation));
      return;
    }
    this.#layer = context.pushTransform(this.needsCompositing, offset, t, (ctx, o) => ctx.paintChild(child, o), this.#layer);
  }
}

/** 矩形裁剪。 */
export class RenderClipRect extends RenderProxyBox {
  #clipBehavior: Clip;
  #layer: ClipRectLayer | null = null;
  constructor(clipBehavior: Clip = "hardEdge", child: RenderBox | null = null) {
    super(child);
    this.#clipBehavior = clipBehavior;
  }
  set clipBehavior(v: Clip) {
    if (v !== this.#clipBehavior) {
      this.#clipBehavior = v;
      this.markNeedsPaint();
    }
  }
  override paint(context: PaintingContext, offset: Offset): void {
    const child = this.child;
    if (child === null) return;
    this.#layer = context.pushClipRect(this.needsCompositing, offset, Rect.fromLTWH(0, 0, this.size.width, this.size.height),
      (ctx, o) => ctx.paintChild(child, o), this.#clipBehavior, this.#layer);
  }
}

/** 圆角矩形裁剪(命中测试也按圆角)。 */
export class RenderClipRRect extends RenderProxyBox {
  #borderRadius: BorderRadius;
  #clipBehavior: Clip;
  #layer: ClipRRectLayer | null = null;
  constructor(borderRadius: BorderRadius = BorderRadius.zero, clipBehavior: Clip = "antiAlias", child: RenderBox | null = null) {
    super(child);
    this.#borderRadius = borderRadius;
    this.#clipBehavior = clipBehavior;
  }
  set borderRadius(v: BorderRadius) {
    if (!v.equals(this.#borderRadius)) {
      this.#borderRadius = v;
      this.markNeedsPaint();
    }
  }
  set clipBehavior(v: Clip) {
    if (v !== this.#clipBehavior) {
      this.#clipBehavior = v;
      this.markNeedsPaint();
    }
  }
  override hitTest(result: HitTestResult, position: Offset): boolean {
    if (this.#clipBehavior !== "none" &&
        !new RRect(Rect.fromLTWH(0, 0, this.size.width, this.size.height), this.#borderRadius).contains(position.dx, position.dy)) {
      return false;
    }
    return super.hitTest(result, position);
  }
  override paint(context: PaintingContext, offset: Offset): void {
    const child = this.child;
    if (child === null) return;
    this.#layer = context.pushClipRRect(this.needsCompositing, offset, Rect.fromLTWH(0, 0, this.size.width, this.size.height),
      this.#borderRadius, (ctx, o) => ctx.paintChild(child, o), this.#clipBehavior, this.#layer);
  }
}

/** 装饰位置。 */
export type DecorationPosition = "background" | "foreground";

/** 在子节点前/后绘制 Decoration。 */
export class RenderDecoratedBox extends RenderProxyBox {
  #decoration: Decoration;
  #position: DecorationPosition;
  constructor(decoration: Decoration, position: DecorationPosition = "background", child: RenderBox | null = null) {
    super(child);
    this.#decoration = decoration;
    this.#position = position;
  }
  get decoration(): Decoration {
    return this.#decoration;
  }
  set decoration(v: Decoration) {
    if (v === this.#decoration) return;
    this.#decoration = v;
    this.markNeedsPaint();
  }
  set position(v: DecorationPosition) {
    if (v !== this.#position) {
      this.#position = v;
      this.markNeedsPaint();
    }
  }
  override hitTestSelf(position: Offset): boolean {
    return this.#decoration.hitTest(this.size, position);
  }
  override paint(context: PaintingContext, offset: Offset): void {
    const rect = Rect.fromLTWH(offset.dx, offset.dy, this.size.width, this.size.height);
    if (this.#position === "background") this.#decoration.paint(context.canvas, rect);
    super.paint(context, offset);
    if (this.#position === "foreground") this.#decoration.paint(context.canvas, rect);
  }
}

/** 指针事件回调。 */
export type PointerEventListener<E extends PointerEvent> = (event: E) => void;

/** 原始指针事件监听。 */
export class RenderPointerListener extends RenderProxyBoxWithHitTestBehavior {
  onPointerDown: PointerEventListener<PointerDownEvent> | null = null;
  onPointerMove: PointerEventListener<PointerMoveEvent> | null = null;
  onPointerUp: PointerEventListener<PointerUpEvent> | null = null;
  onPointerHover: PointerEventListener<PointerHoverEvent> | null = null;
  onPointerCancel: PointerEventListener<PointerCancelEvent> | null = null;
  onPointerSignal: PointerEventListener<PointerSignalEvent> | null = null;

  constructor(behavior: HitTestBehavior = "deferToChild", child: RenderBox | null = null) {
    super(behavior, child);
  }
  protected override computeSizeForNoChild(constraints: BoxConstraints): Size {
    return constraints.biggest;
  }
  override handleEvent(event: PointerEvent, _entry: HitTestEntry): void {
    if (event instanceof PointerDownEvent) this.onPointerDown?.(event);
    else if (event instanceof PointerMoveEvent) this.onPointerMove?.(event);
    else if (event instanceof PointerUpEvent) this.onPointerUp?.(event);
    else if (event instanceof PointerHoverEvent) this.onPointerHover?.(event);
    else if (event instanceof PointerCancelEvent) this.onPointerCancel?.(event);
    else if (event instanceof PointerSignalEvent) this.onPointerSignal?.(event);
  }
}

/** 鼠标区域:enter/exit/hover 回调与光标。 */
export class RenderMouseRegion extends RenderProxyBoxWithHitTestBehavior implements MouseTrackerAnnotation {
  readonly [kMouseTrackerAnnotation] = true as const;
  onEnter: ((event: PointerEnterEvent) => void) | null = null;
  onExit: ((event: PointerExitEvent) => void) | null = null;
  onHover: ((event: PointerHoverEvent) => void) | null = null;
  #cursor: MouseCursor;
  #opaque: boolean;
  #onCursorChanged: (() => void) | null = null;

  constructor(o: { cursor?: MouseCursor; opaque?: boolean } = {}, child: RenderBox | null = null) {
    super((o.opaque ?? true) ? "opaque" : "translucent", child);
    this.#cursor = o.cursor ?? "defer";
    this.#opaque = o.opaque ?? true;
  }
  get cursor(): MouseCursor {
    return this.#cursor;
  }
  set cursor(v: MouseCursor) {
    if (v === this.#cursor) return;
    this.#cursor = v;
    this.#onCursorChanged?.();
  }
  get opaque(): boolean {
    return this.#opaque;
  }
  set opaque(v: boolean) {
    this.#opaque = v;
    this.behavior = v ? "opaque" : "translucent";
  }
  get validForMouseTracker(): boolean {
    return this.attached;
  }
  /** 光标变化时通知(由绑定层用于刷新光标)。 */
  set onCursorChanged(cb: (() => void) | null) {
    this.#onCursorChanged = cb;
  }
  override hitTest(result: HitTestResult, position: Offset): boolean {
    return super.hitTest(result, position) && this.#opaque;
  }
  protected override computeSizeForNoChild(constraints: BoxConstraints): Size {
    return constraints.biggest;
  }
  override handleEvent(event: PointerEvent, _entry: HitTestEntry): void {
    if (event instanceof PointerHoverEvent) this.onHover?.(event);
  }
}

/** 忽略命中(自身与子树对指针不可见)。 */
export class RenderIgnorePointer extends RenderProxyBox {
  #ignoring: boolean;
  constructor(ignoring = true, child: RenderBox | null = null) {
    super(child);
    this.#ignoring = ignoring;
  }
  get ignoring(): boolean {
    return this.#ignoring;
  }
  set ignoring(v: boolean) {
    if (v === this.#ignoring) return;
    this.#ignoring = v;
    // 子树的可命中区域随之出现/消失: 连同子树里各重绘边界的缓存层一起重录。
    const repaint = (node: RenderObject): void => {
      node.markNeedsPaint();
      node.visitChildren(repaint);
    };
    repaint(this);
  }
  override get ignoresPointer(): boolean {
    return this.#ignoring;
  }
  override hitTest(result: HitTestResult, position: Offset): boolean {
    return !this.#ignoring && super.hitTest(result, position);
  }
}

/** 吸收命中(阻止子树与下层收到指针)。 */
export class RenderAbsorbPointer extends RenderProxyBox {
  absorbing: boolean;
  constructor(absorbing = true, child: RenderBox | null = null) {
    super(child);
    this.absorbing = absorbing;
  }
  override hitTest(result: HitTestResult, position: Offset): boolean {
    return this.absorbing ? this.size.contains(position) : super.hitTest(result, position);
  }
  override hitTestSelf(_position: Offset): boolean {
    return this.absorbing;
  }
}

/** 重绘边界:子树单独成层。 */
export class RenderRepaintBoundary extends RenderProxyBox {
  override get isRepaintBoundary(): boolean {
    return true;
  }
}

/** 隐藏(仍保持状态, 不绘制、不命中、尺寸为最小)。 */
export class RenderOffstage extends RenderProxyBox {
  #offstage: boolean;
  constructor(offstage = true, child: RenderBox | null = null) {
    super(child);
    this.#offstage = offstage;
  }
  set offstage(v: boolean) {
    if (v !== this.#offstage) {
      this.#offstage = v;
      this.markNeedsLayout();
    }
  }
  override performLayout(): void {
    if (this.#offstage) {
      this.child?.layout(this.constraints);
      this.size = this.constraints.smallest;
    } else {
      super.performLayout();
    }
  }
  override hitTest(result: HitTestResult, position: Offset): boolean {
    return !this.#offstage && super.hitTest(result, position);
  }
  override paint(context: PaintingContext, offset: Offset): void {
    if (!this.#offstage) super.paint(context, offset);
  }
}

/** 自定义绘制器。 */
export abstract class CustomPainter {
  /** 触发重绘的 Listenable(如动画)。 */
  readonly repaint: Listenable | null;
  constructor(repaint: Listenable | null = null) {
    this.repaint = repaint;
  }
  /** 在 (0,0)-size 区域绘制(canvas 已平移到本地原点)。 */
  abstract paint(canvas: Canvas, size: Size): void;
  /** 新旧 painter 替换时是否需要重绘。 */
  shouldRepaint(_oldDelegate: CustomPainter): boolean {
    return true;
  }
  /** 命中测试;null 表示用默认(命中)。 */
  hitTest(_position: Offset): boolean | null {
    return null;
  }
}

/** 使用 CustomPainter 绘制(背景 painter + 子节点 + 前景 painter)。 */
export class RenderCustomPaint extends RenderProxyBox {
  #painter: CustomPainter | null;
  #foregroundPainter: CustomPainter | null;
  #preferredSize: Size;
  #listener: VoidCallback = () => this.markNeedsPaint();
  constructor(o: { painter?: CustomPainter | null; foregroundPainter?: CustomPainter | null; preferredSize?: Size } = {}, child: RenderBox | null = null) {
    super(child);
    this.#painter = o.painter ?? null;
    this.#foregroundPainter = o.foregroundPainter ?? null;
    this.#preferredSize = o.preferredSize ?? Size.zero;
  }
  get painter(): CustomPainter | null {
    return this.#painter;
  }
  set painter(v: CustomPainter | null) {
    this.#swap(this.#painter, v);
    this.#painter = v;
  }
  set foregroundPainter(v: CustomPainter | null) {
    this.#swap(this.#foregroundPainter, v);
    this.#foregroundPainter = v;
  }
  set preferredSize(v: Size) {
    if (!v.equals(this.#preferredSize)) {
      this.#preferredSize = v;
      this.markNeedsLayout();
    }
  }
  #swap(old: CustomPainter | null, v: CustomPainter | null): void {
    if (old === v) return;
    if (v === null || old === null || v.constructor !== old.constructor || v.shouldRepaint(old)) this.markNeedsPaint();
    if (this.attached) {
      old?.repaint?.removeListener(this.#listener);
      v?.repaint?.addListener(this.#listener);
    }
  }
  override attach(owner: PipelineOwner): void {
    super.attach(owner);
    this.#painter?.repaint?.addListener(this.#listener);
    this.#foregroundPainter?.repaint?.addListener(this.#listener);
  }
  override detach(): void {
    this.#painter?.repaint?.removeListener(this.#listener);
    this.#foregroundPainter?.repaint?.removeListener(this.#listener);
    super.detach();
  }
  protected override computeSizeForNoChild(constraints: BoxConstraints): Size {
    return constraints.constrain(this.#preferredSize);
  }
  override hitTestChildren(result: HitTestResult, position: Offset): boolean {
    if (this.#foregroundPainter !== null && (this.#foregroundPainter.hitTest(position) ?? false)) return true;
    return super.hitTestChildren(result, position);
  }
  override hitTestSelf(position: Offset): boolean {
    return this.#painter !== null && (this.#painter.hitTest(position) ?? true);
  }
  #paintWith(context: PaintingContext, offset: Offset, painter: CustomPainter): void {
    const canvas = context.canvas;
    canvas.save();
    if (offset.dx !== 0 || offset.dy !== 0) canvas.translate(offset.dx, offset.dy);
    painter.paint(canvas, this.size);
    canvas.restore();
  }
  override paint(context: PaintingContext, offset: Offset): void {
    if (this.#painter !== null) this.#paintWith(context, offset, this.#painter);
    super.paint(context, offset);
    if (this.#foregroundPainter !== null) this.#paintWith(context, offset, this.#foregroundPainter);
  }
}

/** 外部纹理(扩展注入)。 */
export class RenderTexture extends RenderBox {
  #textureId: number;
  constructor(textureId: number) {
    super();
    this.#textureId = textureId;
  }
  get textureId(): number {
    return this.#textureId;
  }
  set textureId(v: number) {
    if (v !== this.#textureId) {
      this.#textureId = v;
      this.markNeedsPaint();
    }
  }
  override get sizedByParent(): boolean {
    return true;
  }
  override get alwaysNeedsCompositing(): boolean {
    return true;
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    return constraints.biggest;
  }
  override hitTestSelf(): boolean {
    return true;
  }
  override paint(context: PaintingContext, offset: Offset): void {
    context.addLayer(new TextureLayer(Rect.fromLTWH(offset.dx, offset.dy, this.size.width, this.size.height), this.#textureId));
  }
}
