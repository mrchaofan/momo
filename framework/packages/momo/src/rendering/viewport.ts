import type { HitTestResult } from "../gestures/hit_test";
import { Offset, Rect, Size, type Axis, type Clip } from "../painting/geometry";
import type { Matrix4 } from "../painting/matrix4";
import { ContainerBoxParentData, ContainerRenderBox, RenderBox, RenderBoxWithChild } from "./box";
import { BoxConstraints } from "./box_constraints";
import type { PaintingContext, PipelineOwner, RenderObject } from "./object";
import type { ViewportOffset } from "./viewport_offset";

/**
 * 单子节点滚动视口(SingleChildScrollView):子节点在主轴上不受限布局, 以 -pixels 偏移绘制并裁剪。
 * 滚动只触发重绘(不重新布局)。
 */
export class RenderSingleChildViewport extends RenderBoxWithChild {
  #axis: Axis;
  #offset: ViewportOffset;
  #clipBehavior: Clip;
  #listener = (): void => this.markNeedsPaint();

  constructor(o: { axis?: Axis; offset: ViewportOffset; clipBehavior?: Clip }, child: RenderBox | null = null) {
    super(child);
    this.#axis = o.axis ?? "vertical";
    this.#offset = o.offset;
    this.#clipBehavior = o.clipBehavior ?? "hardEdge";
  }

  get axis(): Axis {
    return this.#axis;
  }
  set axis(v: Axis) {
    if (v !== this.#axis) {
      this.#axis = v;
      this.markNeedsLayout();
    }
  }
  get offset(): ViewportOffset {
    return this.#offset;
  }
  set offset(v: ViewportOffset) {
    if (v === this.#offset) return;
    if (this.attached) {
      this.#offset.removeListener(this.#listener);
      v.addListener(this.#listener);
    }
    this.#offset = v;
    this.markNeedsLayout();
  }
  set clipBehavior(v: Clip) {
    if (v !== this.#clipBehavior) {
      this.#clipBehavior = v;
      this.markNeedsPaint();
    }
  }

  override attach(owner: PipelineOwner): void {
    super.attach(owner);
    this.#offset.addListener(this.#listener);
  }
  override detach(): void {
    this.#offset.removeListener(this.#listener);
    super.detach();
  }

  override get isRepaintBoundary(): boolean {
    return true;
  }

  #innerConstraints(c: BoxConstraints): BoxConstraints {
    return this.#axis === "vertical" ? c.widthConstraints() : c.heightConstraints();
  }

  protected override computeMinIntrinsicWidth(height: number): number {
    return this.child?.getMinIntrinsicWidth(height) ?? 0;
  }
  protected override computeMaxIntrinsicWidth(height: number): number {
    return this.child?.getMaxIntrinsicWidth(height) ?? 0;
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    return this.child?.getMinIntrinsicHeight(width) ?? 0;
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    return this.child?.getMaxIntrinsicHeight(width) ?? 0;
  }

  override performLayout(): void {
    const c = this.constraints;
    const child = this.child;
    if (child === null) {
      this.size = c.smallest;
    } else {
      child.layout(this.#innerConstraints(c), true);
      this.size = c.constrain(child.size);
    }
    const vertical = this.#axis === "vertical";
    const viewport = vertical ? this.size.height : this.size.width;
    const content = child === null ? 0 : vertical ? child.size.height : child.size.width;
    this.#offset.applyViewportDimension(viewport);
    this.#offset.applyContentDimensions(0, Math.max(0, content - viewport));
  }

  /** 子节点的绘制偏移(-pixels)。 */
  get paintOffset(): Offset {
    const p = this.#offset.pixels;
    return this.#axis === "vertical" ? new Offset(0, -p) : new Offset(-p, 0);
  }

  #overflows(): boolean {
    const child = this.child;
    if (child === null) return false;
    return this.#offset.pixels > 0 || child.size.width > this.size.width || child.size.height > this.size.height;
  }

  override paint(context: PaintingContext, offset: Offset): void {
    const child = this.child;
    if (child === null) return;
    const po = this.paintOffset;
    const paintContents = (ctx: PaintingContext, o: Offset): void => ctx.paintChild(child, o.plus(po));
    if (this.#overflows() && this.#clipBehavior !== "none") {
      context.pushClipRect(this.needsCompositing, offset, Rect.fromLTWH(0, 0, this.size.width, this.size.height), paintContents, this.#clipBehavior);
    } else {
      paintContents(context, offset);
    }
  }

  override applyPaintTransform(_child: RenderObject, transform: Matrix4): void {
    const po = this.paintOffset;
    transform.translate(po.dx, po.dy);
  }

  override hitTestChildren(result: HitTestResult, position: Offset): boolean {
    const child = this.child;
    if (child === null) return false;
    return result.addWithPaintOffset(this.paintOffset, position, (r, p) => child.hitTest(r, p));
  }
}

/** 懒加载列表的子节点父数据。 */
export class ListParentData extends ContainerBoxParentData {
  /** 子节点在列表中的下标。 */
  index = 0;
  /** 子节点在内容坐标中的主轴位置。 */
  layoutOffset = 0;
}

/** 懒加载列表的子节点提供者(由 ListView 的 Element 实现)。 */
export interface ListChildManager {
  /** 子节点总数;null 表示由 builder 决定(返回 null 即结束)。 */
  readonly childCount: number | null;
  /** 构建(或更新)index 处的子节点, 并经 insertIndexed 插入视口;不存在时不插入。 */
  createChild(index: number): void;
  /** 移除 index 处的子节点。 */
  removeChild(index: number): void;
}

/**
 * 懒加载列表视口(简化的 RenderSliverList + Viewport):只构建与 [pixels - cacheExtent, pixels + 视口 + cacheExtent]
 * 相交的子项;未测量过的子项按已测量平均高度估算。滚动触发重新布局(只构建/回收增量子项)。
 */
export class RenderListViewport extends ContainerRenderBox<ListParentData> {
  #axis: Axis;
  #offset: ViewportOffset;
  #itemExtent: number | null;
  #cacheExtent: number;
  #clipBehavior: Clip;
  #manager: ListChildManager | null = null;
  #byIndex = new Map<number, RenderBox>();
  #extents = new Map<number, number>();
  #measuredSum = 0;
  #knownCount: number | null = null;
  #listener = (): void => this.markNeedsLayout();

  constructor(o: { axis?: Axis; offset: ViewportOffset; itemExtent?: number | null; cacheExtent?: number; clipBehavior?: Clip }) {
    super();
    this.#axis = o.axis ?? "vertical";
    this.#offset = o.offset;
    this.#itemExtent = o.itemExtent ?? null;
    this.#cacheExtent = o.cacheExtent ?? 250;
    this.#clipBehavior = o.clipBehavior ?? "hardEdge";
  }

  protected createParentData(): ListParentData {
    return new ListParentData();
  }

  /** 子节点提供者(Element 在 mount 时设置)。 */
  set childManager(v: ListChildManager | null) {
    this.#manager = v;
  }

  get axis(): Axis {
    return this.#axis;
  }
  set axis(v: Axis) {
    if (v !== this.#axis) {
      this.#axis = v;
      this.#extents.clear();
      this.#measuredSum = 0;
      this.markNeedsLayout();
    }
  }
  get offset(): ViewportOffset {
    return this.#offset;
  }
  set offset(v: ViewportOffset) {
    if (v === this.#offset) return;
    if (this.attached) {
      this.#offset.removeListener(this.#listener);
      v.addListener(this.#listener);
    }
    this.#offset = v;
    this.markNeedsLayout();
  }
  set itemExtent(v: number | null) {
    if (v !== this.#itemExtent) {
      this.#itemExtent = v;
      this.markNeedsLayout();
    }
  }
  set cacheExtent(v: number) {
    if (v !== this.#cacheExtent) {
      this.#cacheExtent = v;
      this.markNeedsLayout();
    }
  }
  set clipBehavior(v: Clip) {
    if (v !== this.#clipBehavior) {
      this.#clipBehavior = v;
      this.markNeedsPaint();
    }
  }

  /** 数据变化(子项数/内容)后调用:清除尺寸缓存并重新布局。 */
  markChildrenDirty(): void {
    this.#extents.clear();
    this.#measuredSum = 0;
    this.#knownCount = null;
    this.markNeedsLayout();
  }

  override attach(owner: PipelineOwner): void {
    super.attach(owner);
    this.#offset.addListener(this.#listener);
  }
  override detach(): void {
    this.#offset.removeListener(this.#listener);
    super.detach();
  }

  override get isRepaintBoundary(): boolean {
    return true;
  }

  /** 按下标插入子节点(保持 children 按 index 有序)。 */
  insertIndexed(child: RenderBox, index: number): void {
    this.setupParentData(child);
    (child.parentData as ListParentData).index = index;
    let after: RenderBox | null = null;
    for (const c of this.children) {
      if ((c.parentData as ListParentData).index < index) after = c;
      else break;
    }
    this.insert(child, after);
    (child.parentData as ListParentData).index = index;
    this.#byIndex.set(index, child);
  }

  override remove(child: RenderBox): void {
    const pd = child.parentData as ListParentData | null;
    if (pd !== null && this.#byIndex.get(pd.index) === child) this.#byIndex.delete(pd.index);
    super.remove(child);
  }

  /** index 处当前存在的子节点。 */
  childAtIndex(index: number): RenderBox | null {
    return this.#byIndex.get(index) ?? null;
  }

  /** 当前已构建的下标(调试/测试)。 */
  get builtIndices(): number[] {
    return [...this.#byIndex.keys()].sort((a, b) => a - b);
  }

  #main(s: Size): number {
    return this.#axis === "vertical" ? s.height : s.width;
  }

  /** 子项位置估算(未测量者用平均尺寸)。 */
  #estimate(): number {
    if (this.#itemExtent !== null) return this.#itemExtent;
    return this.#extents.size > 0 ? this.#measuredSum / this.#extents.size : 48;
  }

  #setExtent(index: number, extent: number): void {
    const old = this.#extents.get(index);
    if (old !== undefined) this.#measuredSum -= old;
    this.#extents.set(index, extent);
    this.#measuredSum += extent;
  }

  override performLayout(): void {
    const c = this.constraints;
    const vertical = this.#axis === "vertical";
    const shrinkWrap = vertical ? !c.hasBoundedHeight : !c.hasBoundedWidth;
    const childConstraints = vertical
      ? new BoxConstraints(c.maxWidth === Infinity ? 0 : c.maxWidth, c.maxWidth, 0, Infinity)
      : new BoxConstraints(0, Infinity, c.maxHeight === Infinity ? 0 : c.maxHeight, c.maxHeight);
    let total = 0;
    for (let attempt = 0; attempt < 4; attempt++) {
      const viewportMain = shrinkWrap ? Infinity : vertical ? c.maxHeight : c.maxWidth;
      const pixels = shrinkWrap ? 0 : this.#offset.pixels;
      const start = pixels - this.#cacheExtent;
      const end = pixels + viewportMain + this.#cacheExtent;
      const keep = new Set<number>();
      let pos = 0;
      this.invokeLayoutCallback(() => {
        const manager = this.#manager;
        let count = manager?.childCount ?? this.#knownCount;
        let index = 0;
        while (count === null || index < count) {
          let extent = this.#itemExtent ?? this.#extents.get(index) ?? this.#estimate();
          if (pos + extent >= start && pos <= end) {
            let child = this.#byIndex.get(index) ?? null;
            if (child === null && manager !== null) {
              manager.createChild(index);
              child = this.#byIndex.get(index) ?? null;
            }
            if (child === null) {
              count = index;
              this.#knownCount = index;
              break;
            }
            child.layout(this.#itemExtent !== null
              ? (vertical ? childConstraints.tighten({ height: this.#itemExtent }) : childConstraints.tighten({ width: this.#itemExtent }))
              : childConstraints, true);
            extent = this.#main(child.size);
            this.#setExtent(index, extent);
            const pd = child.parentData as ListParentData;
            pd.layoutOffset = pos;
            pd.offset = vertical ? new Offset(0, pos) : new Offset(pos, 0);
            keep.add(index);
          } else if (count === null && pos > end) {
            // 无限列表:视口之后不再遍历, 预留一屏可继续滚动。
            pos += Number.isFinite(viewportMain) ? viewportMain : 0;
            break;
          }
          pos += extent;
          index++;
        }
        for (const idx of [...this.#byIndex.keys()]) {
          if (!keep.has(idx)) {
            if (manager !== null) manager.removeChild(idx);
            else {
              const child = this.#byIndex.get(idx);
              if (child) this.remove(child);
            }
          }
        }
      });
      total = pos;
      const size = shrinkWrap
        ? c.constrain(vertical ? new Size(c.maxWidth === Infinity ? 0 : c.maxWidth, total) : new Size(total, c.maxHeight === Infinity ? 0 : c.maxHeight))
        : c.biggest;
      this.size = size;
      const vp = this.#main(size);
      const okViewport = this.#offset.applyViewportDimension(vp);
      const okContent = this.#offset.applyContentDimensions(0, Math.max(0, total - vp));
      if (okViewport && okContent) break;
    }
  }

  /** 子节点的绘制位置(局部坐标, 已减去滚动)。 */
  childPaintOffset(child: RenderBox): Offset {
    const pd = child.parentData as ListParentData;
    const p = this.#offset.pixels;
    return this.#axis === "vertical" ? new Offset(0, pd.layoutOffset - p) : new Offset(pd.layoutOffset - p, 0);
  }

  override paint(context: PaintingContext, offset: Offset): void {
    const paintContents = (ctx: PaintingContext, o: Offset): void => {
      const vp = this.#main(this.size);
      for (const child of this.children) {
        const po = this.childPaintOffset(child);
        const lead = this.#axis === "vertical" ? po.dy : po.dx;
        if (lead + this.#main(child.size) <= 0 || lead >= vp) continue;
        ctx.paintChild(child, new Offset(o.dx + po.dx, o.dy + po.dy));
      }
    };
    if (this.#clipBehavior !== "none") {
      context.pushClipRect(this.needsCompositing, offset, Rect.fromLTWH(0, 0, this.size.width, this.size.height), paintContents, this.#clipBehavior);
    } else {
      paintContents(context, offset);
    }
  }

  override applyPaintTransform(child: RenderObject, transform: Matrix4): void {
    const po = this.childPaintOffset(child as RenderBox);
    transform.translate(po.dx, po.dy);
  }

  override hitTestChildren(result: HitTestResult, position: Offset): boolean {
    for (let i = this.children.length - 1; i >= 0; i--) {
      const child = this.children[i]!;
      if (result.addWithPaintOffset(this.childPaintOffset(child), position, (r, p) => child.hitTest(r, p))) return true;
    }
    return false;
  }
}
