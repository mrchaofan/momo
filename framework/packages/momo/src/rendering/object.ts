import { PictureRecorder, type Canvas } from "momo:ui";
import { reportError } from "../foundation/assert";
import { describeIdentity } from "../foundation/diagnostics";
import type { PointerEvent } from "../gestures/events";
import type { HitTestEntry, HitTestTarget } from "../gestures/hit_test";
import { BorderRadius, clipRRect } from "../painting/borders";
import { Offset, Rect, type Clip } from "../painting/geometry";
import { Matrix4 } from "../painting/matrix4";
import type { Constraints } from "./box_constraints";
import {
  ClipRRectLayer, ClipRectLayer, ContainerLayer, Layer, OffsetLayer, OpacityLayer, PictureLayer, TransformLayer,
} from "./layer";

/** 父节点存放在子节点上的数据(位置、flex 等)。 */
export class ParentData {
  /** 子节点脱离父节点时调用。 */
  detach(): void {}
}

/** 绘制回调。 */
export type PaintingContextCallback = (context: PaintingContext, offset: Offset) => void;

/**
 * 绘制上下文:管理当前 PictureLayer 的录制, 子 RepaintBoundary 合成为独立层,
 * push* 系列在需要合成时创建对应 Layer, 否则直接在 Canvas 上 save/clip/restore。
 */
export class PaintingContext {
  #containerLayer: ContainerLayer;
  readonly estimatedBounds: Rect;
  #currentLayer: PictureLayer | null = null;
  #recorder: PictureRecorder | null = null;
  #canvas: Canvas | null = null;
  /** 正在绘制被忽略命中的子树(IgnorePointer): 其中的节点不记录可命中区域。 */
  ignoringPointer = false;

  constructor(containerLayer: ContainerLayer, estimatedBounds: Rect) {
    this.#containerLayer = containerLayer;
    this.estimatedBounds = estimatedBounds;
  }

  /** 重绘一个 RepaintBoundary(复用其 OffsetLayer)。 */
  static repaintCompositedChild(child: RenderObject): void {
    let layer = child._layer;
    if (layer === null) {
      layer = new OffsetLayer();
      child._layer = layer;
    } else {
      layer.removeAllChildren();
    }
    const ctx = new PaintingContext(layer, child.paintBounds);
    for (let p = child.parent; p !== null && !ctx.ignoringPointer; p = p.parent) ctx.ignoringPointer = p.ignoresPointer;
    child._paintWithContext(ctx, Offset.zero);
    ctx.stopRecordingIfNeeded();
  }

  /** 当前录制的 Canvas(惰性开始录制)。 */
  get canvas(): Canvas {
    if (this.#canvas === null) this.#startRecording();
    return this.#canvas!;
  }

  #startRecording(): void {
    const b = this.estimatedBounds;
    this.#currentLayer = new PictureLayer(b);
    this.#recorder = new PictureRecorder();
    this.#canvas = Number.isFinite(b.width) && Number.isFinite(b.height)
      ? this.#recorder.beginRecording(b.left, b.top, b.width, b.height)
      : this.#recorder.beginRecording();
    this.#containerLayer.append(this.#currentLayer);
  }

  /** 结束当前 Picture 录制。 */
  stopRecordingIfNeeded(): void {
    if (this.#canvas === null) return;
    this.#currentLayer!.picture = this.#recorder!.endRecording();
    this.#currentLayer = null;
    this.#recorder = null;
    this.#canvas = null;
  }

  /** 绘制子节点(RepaintBoundary 子节点作为独立层合成)。 */
  paintChild(child: RenderObject, offset: Offset): void {
    if (child.isRepaintBoundary) {
      this.stopRecordingIfNeeded();
      if (child._needsPaint || child._layer === null) PaintingContext.repaintCompositedChild(child);
      const layer = child._layer!;
      layer.offset = offset;
      this.appendLayer(layer);
    } else {
      child._paintWithContext(this, offset);
    }
  }

  /** 追加层(不结束录制;调用方负责)。 */
  appendLayer(layer: Layer): void {
    layer.remove();
    this.#containerLayer.append(layer);
  }

  /** 结束录制后追加一个叶子层(如 TextureLayer)。 */
  addLayer(layer: Layer): void {
    this.stopRecordingIfNeeded();
    this.appendLayer(layer);
  }

  /** 把 childLayer 作为容器, 在其中执行 painter。 */
  pushLayer(childLayer: ContainerLayer, painter: PaintingContextCallback, offset: Offset, childPaintBounds?: Rect): void {
    this.stopRecordingIfNeeded();
    childLayer.removeAllChildren();
    this.appendLayer(childLayer);
    const ctx = new PaintingContext(childLayer, childPaintBounds ?? this.estimatedBounds);
    ctx.ignoringPointer = this.ignoringPointer;
    painter(ctx, offset);
    ctx.stopRecordingIfNeeded();
  }

  /** 矩形裁剪(clipRect 为局部坐标, 会加上 offset)。 */
  pushClipRect(needsCompositing: boolean, offset: Offset, clipRect: Rect, painter: PaintingContextCallback,
    clipBehavior: Clip = "hardEdge", oldLayer: ClipRectLayer | null = null): ClipRectLayer | null {
    if (clipBehavior === "none") {
      painter(this, offset);
      return null;
    }
    const r = clipRect.shift(offset);
    if (needsCompositing) {
      const layer = oldLayer ?? new ClipRectLayer();
      layer.clipRect = r;
      this.pushLayer(layer, painter, offset, r);
      return layer;
    }
    const c = this.canvas;
    c.save();
    c.clipRect(r.left, r.top, r.width, r.height);
    painter(this, offset);
    this.canvas.restore();
    return null;
  }

  /** 圆角矩形裁剪。 */
  pushClipRRect(needsCompositing: boolean, offset: Offset, clipRect: Rect, borderRadius: BorderRadius,
    painter: PaintingContextCallback, clipBehavior: Clip = "antiAlias", oldLayer: ClipRRectLayer | null = null): ClipRRectLayer | null {
    if (clipBehavior === "none") {
      painter(this, offset);
      return null;
    }
    const r = clipRect.shift(offset);
    if (needsCompositing) {
      const layer = oldLayer ?? new ClipRRectLayer();
      layer.clipRect = r;
      layer.borderRadius = borderRadius;
      this.pushLayer(layer, painter, offset, r);
      return layer;
    }
    const c = this.canvas;
    c.save();
    clipRRect(c, r, borderRadius);
    painter(this, offset);
    this.canvas.restore();
    return null;
  }

  /** 变换(transform 以 offset 为原点)。 */
  pushTransform(needsCompositing: boolean, offset: Offset, transform: Matrix4, painter: PaintingContextCallback,
    oldLayer: TransformLayer | null = null): TransformLayer | null {
    const effective = Matrix4.translationValues(offset.dx, offset.dy).multiply(transform).translate(-offset.dx, -offset.dy);
    if (needsCompositing) {
      const layer = oldLayer ?? new TransformLayer();
      layer.transform = effective;
      layer.offset = Offset.zero;
      const inverse = effective.inverted();
      this.pushLayer(layer, painter, offset, inverse ? inverse.transformRect(this.estimatedBounds) : this.estimatedBounds);
      return layer;
    }
    const c = this.canvas;
    c.save();
    c.transform(effective.storage);
    painter(this, offset);
    this.canvas.restore();
    return null;
  }

  /** 透明度(alpha 0..255)层。 */
  pushOpacity(offset: Offset, alpha: number, painter: PaintingContextCallback, oldLayer: OpacityLayer | null = null): OpacityLayer {
    const layer = oldLayer ?? new OpacityLayer();
    layer.alpha = alpha;
    layer.offset = offset;
    this.pushLayer(layer, painter, Offset.zero);
    return layer;
  }
}

/**
 * 布局/绘制/合成脏节点的管理者(对标 Flutter PipelineOwner)。
 */
export class PipelineOwner {
  /** 有节点变脏、需要新帧时调用。 */
  onNeedVisualUpdate: (() => void) | null;
  #rootNode: RenderObject | null = null;
  /** @internal */ _nodesNeedingLayout: RenderObject[] = [];
  /** @internal */ _nodesNeedingCompositingBitsUpdate: RenderObject[] = [];
  /** @internal */ _nodesNeedingPaint: RenderObject[] = [];
  /** 布局回调(如 ListView 布局期构建子节点)执行中。 */
  debugDoingLayout = false;

  constructor(onNeedVisualUpdate: (() => void) | null = null) {
    this.onNeedVisualUpdate = onNeedVisualUpdate;
  }

  get rootNode(): RenderObject | null {
    return this.#rootNode;
  }
  set rootNode(v: RenderObject | null) {
    if (this.#rootNode === v) return;
    this.#rootNode?.detach();
    this.#rootNode = v;
    v?.attach(this);
  }

  requestVisualUpdate(): void {
    this.onNeedVisualUpdate?.();
  }

  /** 是否有待处理的布局/绘制。 */
  get hasDirtyNodes(): boolean {
    return this._nodesNeedingLayout.length > 0 || this._nodesNeedingPaint.length > 0 || this._nodesNeedingCompositingBitsUpdate.length > 0;
  }

  /** 布局所有脏的 relayout boundary(浅到深)。 */
  flushLayout(): void {
    this.debugDoingLayout = true;
    try {
      while (this._nodesNeedingLayout.length > 0) {
        const dirty = this._nodesNeedingLayout;
        this._nodesNeedingLayout = [];
        dirty.sort((a, b) => a.depth - b.depth);
        for (let i = 0; i < dirty.length; i++) {
          const node = dirty[i]!;
          if (node._needsLayout && node.owner === this) node._layoutWithoutResize();
        }
      }
    } finally {
      this.debugDoingLayout = false;
    }
  }

  /** 更新 needsCompositing 位。 */
  flushCompositingBits(): void {
    const dirty = this._nodesNeedingCompositingBitsUpdate;
    this._nodesNeedingCompositingBitsUpdate = [];
    dirty.sort((a, b) => a.depth - b.depth);
    for (let i = 0; i < dirty.length; i++) {
      const node = dirty[i]!;
      if (node._needsCompositingBitsUpdate && node.owner === this) node._updateCompositingBits();
    }
  }

  /** 重绘脏的 repaint boundary(深到浅)。 */
  flushPaint(): void {
    const dirty = this._nodesNeedingPaint;
    this._nodesNeedingPaint = [];
    dirty.sort((a, b) => b.depth - a.depth);
    for (let i = 0; i < dirty.length; i++) {
      const node = dirty[i]!;
      if (node._needsPaint && node.owner === this && node._layer !== null) {
        PaintingContext.repaintCompositedChild(node);
      }
    }
  }
}

/**
 * 渲染树节点基类:布局(约束下行、尺寸上行、单遍)、绘制、命中测试。
 * 下划线成员为 PipelineOwner/PaintingContext 内部协作使用。
 */
export abstract class RenderObject implements HitTestTarget {
  parentData: ParentData | null = null;
  parent: RenderObject | null = null;
  depth = 0;
  #owner: PipelineOwner | null = null;

  /** @internal */ _needsLayout = true;
  /** @internal */ _relayoutBoundary: RenderObject | null = null;
  /** @internal */ _constraints: Constraints | null = null;
  /** @internal */ _needsCompositingBitsUpdate = true;
  /** @internal */ _needsCompositing = false;
  /** @internal */ _needsPaint = true;
  /** @internal RepaintBoundary 的合成层。 */ _layer: OffsetLayer | null = null;
  #disposed = false;

  get owner(): PipelineOwner | null {
    return this.#owner;
  }
  get attached(): boolean {
    return this.#owner !== null;
  }
  get debugDisposed(): boolean {
    return this.#disposed;
  }

  /** 为子节点准备 parentData(子类覆盖以使用特定类型)。 */
  setupParentData(child: RenderObject): void {
    if (!(child.parentData instanceof ParentData)) child.parentData = new ParentData();
  }

  /** 遍历直接子节点。 */
  visitChildren(_visitor: (child: RenderObject) => void): void {}

  /** 收养子节点。 */
  adoptChild(child: RenderObject): void {
    this.setupParentData(child);
    this.markNeedsLayout();
    this.markNeedsCompositingBitsUpdate();
    child.parent = this;
    if (this.#owner !== null) child.attach(this.#owner);
    this.redepthChild(child);
  }

  /** 放弃子节点。 */
  dropChild(child: RenderObject): void {
    child._cleanRelayoutBoundary();
    child.parentData?.detach();
    child.parent = null;
    if (this.attached) child.detach();
    this.markNeedsLayout();
    this.markNeedsCompositingBitsUpdate();
  }

  redepthChild(child: RenderObject): void {
    if (child.depth <= this.depth) {
      child.depth = this.depth + 1;
      child.redepthChildren();
    }
  }
  redepthChildren(): void {
    this.visitChildren((c) => this.redepthChild(c));
  }

  attach(owner: PipelineOwner): void {
    this.#owner = owner;
    if (this._needsLayout && this._relayoutBoundary !== null) {
      this._needsLayout = false;
      this.markNeedsLayout();
    }
    if (this._needsCompositingBitsUpdate) {
      this._needsCompositingBitsUpdate = false;
      this.markNeedsCompositingBitsUpdate();
    }
    if (this._needsPaint && this._layer !== null) {
      this._needsPaint = false;
      this.markNeedsPaint();
    }
    this.visitChildren((c) => c.attach(owner));
  }

  detach(): void {
    this.#owner = null;
    this.visitChildren((c) => c.detach());
  }

  /** 释放资源(层等)。 */
  dispose(): void {
    this._layer?.remove();
    this._layer = null;
    this.#disposed = true;
  }

  // ---------------------------------------------------------------------------
  // 布局

  get constraints(): Constraints {
    if (this._constraints === null) throw new Error(`${describeIdentity(this)} has no constraints yet`);
    return this._constraints;
  }

  /** 尺寸只由约束决定(performResize 中计算)。 */
  get sizedByParent(): boolean {
    return false;
  }

  get needsLayout(): boolean {
    return this._needsLayout;
  }

  /** 标记需要重新布局(传播到 relayout boundary)。 */
  markNeedsLayout(): void {
    if (this._needsLayout) return;
    if (this._relayoutBoundary === null) {
      this._needsLayout = true;
      if (this.parent !== null) this.markParentNeedsLayout();
      return;
    }
    if (this._relayoutBoundary !== this) {
      this.markParentNeedsLayout();
    } else {
      this._needsLayout = true;
      const owner = this.#owner;
      if (owner !== null) {
        owner._nodesNeedingLayout.push(this);
        owner.requestVisualUpdate();
      }
    }
  }

  markParentNeedsLayout(): void {
    this._needsLayout = true;
    this.parent?.markNeedsLayout();
  }

  /** @internal */
  _cleanRelayoutBoundary(): void {
    if (this._relayoutBoundary !== this) {
      this._relayoutBoundary = null;
      this.visitChildren((c) => c._cleanRelayoutBoundary());
    }
  }

  /** 根节点首次布局登记。 */
  scheduleInitialLayout(): void {
    this._relayoutBoundary = this;
    this.#owner?._nodesNeedingLayout.push(this);
  }

  /** @internal */
  _layoutWithoutResize(): void {
    try {
      this.performLayout();
    } catch (e) {
      reportError(e, `during performLayout() of ${describeIdentity(this)}`);
    }
    this._needsLayout = false;
    this.markNeedsPaint();
  }

  /**
   * 以 constraints 布局。parentUsesSize=false 时本节点成为 relayout boundary(父不依赖其尺寸)。
   */
  layout(constraints: Constraints, parentUsesSize = false): void {
    const isBoundary = !parentUsesSize || this.sizedByParent || constraints.isTight || this.parent === null;
    const boundary = isBoundary ? this : this.parent!._relayoutBoundary;
    if (!this._needsLayout && constraints.equals(this._constraints)) {
      if (boundary !== this._relayoutBoundary) {
        this._relayoutBoundary = boundary;
        this.visitChildren((c) => c._propagateRelayoutBoundary());
      }
      return;
    }
    this._constraints = constraints;
    if (this._relayoutBoundary !== null && boundary !== this._relayoutBoundary) {
      this.visitChildren((c) => c._cleanRelayoutBoundary());
    }
    this._relayoutBoundary = boundary;
    if (this.sizedByParent) {
      try {
        this.performResize();
      } catch (e) {
        reportError(e, `during performResize() of ${describeIdentity(this)}`);
      }
    }
    try {
      this.performLayout();
    } catch (e) {
      reportError(e, `during performLayout() of ${describeIdentity(this)}`);
    }
    this._needsLayout = false;
    this.markNeedsPaint();
  }

  /** @internal */
  _propagateRelayoutBoundary(): void {
    if (this._relayoutBoundary === this) return;
    const parentBoundary = this.parent?._relayoutBoundary ?? null;
    if (parentBoundary !== this._relayoutBoundary) {
      this._relayoutBoundary = parentBoundary;
      this.visitChildren((c) => c._propagateRelayoutBoundary());
    }
  }

  /** sizedByParent 时仅依据约束确定尺寸。 */
  performResize(): void {}

  /** 计算布局(子类实现)。 */
  abstract performLayout(): void;

  /** 在布局期间执行回调(允许修改子树, 如懒构建列表项)。 */
  invokeLayoutCallback(callback: () => void): void {
    callback();
  }

  // ---------------------------------------------------------------------------
  // 合成与绘制

  /** 是否单独成层(重绘隔离)。 */
  get isRepaintBoundary(): boolean {
    return false;
  }

  /** 是否总是需要合成层(如透明度、纹理)。 */
  get alwaysNeedsCompositing(): boolean {
    return false;
  }

  /** 子树是否包含需要合成的节点。 */
  get needsCompositing(): boolean {
    return this._needsCompositing;
  }

  markNeedsCompositingBitsUpdate(): void {
    if (this._needsCompositingBitsUpdate) return;
    this._needsCompositingBitsUpdate = true;
    const parent = this.parent;
    if (parent !== null) {
      if (parent._needsCompositingBitsUpdate) return;
      if (!this.isRepaintBoundary && !parent.isRepaintBoundary) {
        parent.markNeedsCompositingBitsUpdate();
        return;
      }
    }
    this.#owner?._nodesNeedingCompositingBitsUpdate.push(this);
  }

  /** @internal */
  _updateCompositingBits(): void {
    if (!this._needsCompositingBitsUpdate) return;
    const old = this._needsCompositing;
    let needs = false;
    this.visitChildren((child) => {
      child._updateCompositingBits();
      if (child.needsCompositing) needs = true;
    });
    if (this.isRepaintBoundary || this.alwaysNeedsCompositing) needs = true;
    this._needsCompositing = needs;
    this._needsCompositingBitsUpdate = false;
    if (old !== needs) {
      this._needsPaint = false;
      this.markNeedsPaint();
    }
  }

  get needsPaint(): boolean {
    return this._needsPaint;
  }

  /** 标记需要重绘(传播到 repaint boundary)。 */
  markNeedsPaint(): void {
    if (this._needsPaint) return;
    this._needsPaint = true;
    if (this.isRepaintBoundary) {
      const owner = this.#owner;
      if (owner !== null) {
        owner._nodesNeedingPaint.push(this);
        owner.requestVisualUpdate();
      }
    } else if (this.parent !== null) {
      this.parent.markNeedsPaint();
    } else {
      this.#owner?.requestVisualUpdate();
    }
  }

  /** @internal */
  _paintWithContext(context: PaintingContext, offset: Offset): void {
    if (this._needsLayout) return;
    this._needsPaint = false;
    const outer = context.ignoringPointer;
    if (this.ignoresPointer) context.ignoringPointer = true;
    try {
      if (!context.ignoringPointer) this.paintHitRegion(context, offset);
      this.paint(context, offset);
    } catch (e) {
      reportError(e, `during paint() of ${describeIdentity(this)}`);
    } finally {
      context.ignoringPointer = outer;
    }
  }

  /** 本节点及其子树对指针不可见(IgnorePointer)。改变时须重绘子树(可命中区域随之变化)。 */
  get ignoresPointer(): boolean {
    return false;
  }

  /**
   * 记录本节点自身的可命中区域(Canvas.addHitRegion, 包围盒)。宿主据此判断平台视图上方是否有 momo 内容
   * 接收指针(docs/05 §9)。在 paint 之前调用, 因此区域与本节点的绘制同属一层、同一 z 序。
   */
  paintHitRegion(_context: PaintingContext, _offset: Offset): void {}

  /** 局部绘制范围(RepaintBoundary 用作录制的 cull rect)。 */
  abstract get paintBounds(): Rect;

  /** 绘制(offset 为本节点在 context 坐标中的原点)。 */
  paint(_context: PaintingContext, _offset: Offset): void {}

  /** 把子节点坐标到本节点坐标的变换右乘进 transform。 */
  applyPaintTransform(_child: RenderObject, _transform: Matrix4): void {}

  /** 本节点坐标 → ancestor(null = 根)坐标的变换。 */
  getTransformTo(ancestor: RenderObject | null): Matrix4 {
    const chain: RenderObject[] = [this];
    let n: RenderObject = this;
    while (n !== ancestor && n.parent !== null) {
      n = n.parent;
      chain.push(n);
    }
    const t = new Matrix4();
    for (let i = chain.length - 1; i > 0; i--) chain[i]!.applyPaintTransform(chain[i - 1]!, t);
    return t;
  }

  // ---------------------------------------------------------------------------
  // 事件

  /** 命中后接收指针事件。 */
  handleEvent(_event: PointerEvent, _entry: HitTestEntry): void {}

  /** 调试:一行描述。 */
  toStringShort(): string {
    return describeIdentity(this);
  }

  /** 调试:子树描述。 */
  toStringDeep(indent = ""): string {
    let s = `${indent}${this.toStringShort()}${this.debugDescribe()}\n`;
    this.visitChildren((c) => {
      s += c.toStringDeep(indent + "  ");
    });
    return s;
  }

  /** 调试:附加属性描述。 */
  debugDescribe(): string {
    return "";
  }
}
