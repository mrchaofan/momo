import type { Scene, SceneBuilder } from "momo:ui";
import { HitTestEntry, HitTestResult } from "../gestures/hit_test";
import { Offset, Rect, Size } from "../painting/geometry";
import { Matrix4 } from "../painting/matrix4";
import { RenderBox } from "./box";
import { BoxConstraints } from "./box_constraints";
import { TransformLayer } from "./layer";
import { PaintingContext, RenderObject } from "./object";

/** 视图配置:逻辑尺寸 + 设备像素比。 */
export interface ViewConfiguration {
  readonly size: Size;
  readonly devicePixelRatio: number;
}

/**
 * 渲染树根:以逻辑像素布局子节点(紧约束 = 窗口逻辑尺寸), 根层为按 devicePixelRatio 缩放的 TransformLayer,
 * 合成出的 Scene 根坐标系为物理像素。
 */
export class RenderView extends RenderObject {
  #child: RenderBox | null = null;
  #configuration: ViewConfiguration;
  #size: Size = Size.zero;

  constructor(configuration: ViewConfiguration, child: RenderBox | null = null) {
    super();
    this.#configuration = configuration;
    this.child = child;
  }

  get child(): RenderBox | null {
    return this.#child;
  }
  set child(v: RenderBox | null) {
    if (this.#child === v) return;
    if (this.#child !== null) this.dropChild(this.#child);
    this.#child = v;
    if (v !== null) this.adoptChild(v);
  }

  override visitChildren(visitor: (child: RenderObject) => void): void {
    if (this.#child !== null) visitor(this.#child);
  }

  get configuration(): ViewConfiguration {
    return this.#configuration;
  }
  set configuration(v: ViewConfiguration) {
    const old = this.#configuration;
    this.#configuration = v;
    if (this._layer !== null && old.devicePixelRatio !== v.devicePixelRatio) {
      (this._layer as TransformLayer).transform = RenderView.#rootTransform(v.devicePixelRatio);
    }
    if (!old.size.equals(v.size) || old.devicePixelRatio !== v.devicePixelRatio) {
      this.markNeedsLayout();
      this._needsPaint = false;
      this.markNeedsPaint();
    }
  }

  /** 逻辑尺寸。 */
  get size(): Size {
    return this.#size;
  }

  static #rootTransform(dpr: number): Matrix4 {
    return Matrix4.diagonal3Values(dpr, dpr, 1);
  }

  /** 首帧前调用:登记初始布局与绘制。 */
  prepareInitialFrame(): void {
    this.scheduleInitialLayout();
    const layer = new TransformLayer(RenderView.#rootTransform(this.#configuration.devicePixelRatio));
    this._layer = layer;
    this.owner?._nodesNeedingPaint.push(this);
  }

  override get isRepaintBoundary(): boolean {
    return true;
  }

  override performLayout(): void {
    this.#size = this.#configuration.size;
    this.#child?.layout(BoxConstraints.tight(this.#size));
  }

  override get paintBounds(): Rect {
    return Rect.fromLTWH(0, 0, this.#size.width, this.#size.height);
  }

  override paint(context: PaintingContext, offset: Offset): void {
    if (this.#child !== null) context.paintChild(this.#child, offset);
  }

  /**
   * 在逻辑坐标 position 命中测试。
   * runApp 同步建好渲染树, 但布局要等首帧;窗口显示后、首帧前就可能到来指针事件(鼠标停在窗口上时的
   * add/hover), 此时子树尚无尺寸, 不参与命中测试(帧与帧之间渲染树总是已布局的, 只有首帧前需要兜底)。
   */
  hitTest(result: HitTestResult, position: Offset): boolean {
    if (this.#child !== null && this.#child.hasSize) this.#child.hitTest(result, position);
    result.add(new HitTestEntry(this));
    return true;
  }

  /** 合成层树 → Scene。 */
  compositeFrame(builder: SceneBuilder): Scene {
    const layer = this._layer;
    if (layer !== null) layer.addToScene(builder);
    return builder.build();
  }

  /** 根层(调试/测试)。 */
  get rootLayer(): TransformLayer | null {
    return this._layer as TransformLayer | null;
  }
}
