import { Offset, Rect, Size } from "../painting/geometry";
import { RenderBox } from "./box";
import { BoxConstraints } from "./box_constraints";
import { PlatformViewLayer } from "./layer";
import type { PaintingContext } from "./object";

/**
 * 平台视图(扩展创建的系统原生视图, 如 WKWebView)在渲染树中的占位(docs/05 §9)。
 * 尺寸取约束的最大值;绘制时追加一个 PlatformViewLayer, 引擎合成时把原生视图摆到这个位置,
 * 之后绘制的 momo 内容叠在它上面。
 * 指针事件由原生视图直接处理(embedder 的命中测试), 一般不会到达这里;仍让自身可命中,
 * 避免视图尚未就绪或被遮挡区域外的事件穿透到下面的组件。
 */
export class RenderPlatformView extends RenderBox {
  #viewId: number;

  constructor(viewId: number) {
    super();
    this.#viewId = viewId;
  }

  get viewId(): number {
    return this.#viewId;
  }
  set viewId(v: number) {
    if (v !== this.#viewId) {
      this.#viewId = v;
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
    if (this.size.width <= 0 || this.size.height <= 0) return;
    context.addLayer(new PlatformViewLayer(Rect.fromLTWH(offset.dx, offset.dy, this.size.width, this.size.height), this.#viewId));
  }
}
