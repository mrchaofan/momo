import { Offset, Rect, Size } from "../painting/geometry";
import { RenderBox } from "./box";
import { BoxConstraints } from "./box_constraints";
import { PlatformViewLayer } from "./layer";
import type { PaintingContext } from "./object";

/**
 * 平台视图(扩展创建的系统原生视图, 如 WKWebView)在渲染树中的占位(docs/05 §9)。
 * 尺寸取约束的最大值;绘制时追加一个 PlatformViewLayer, 引擎合成时把原生视图摆到这个位置,
 * 之后绘制的 momo 内容叠在它上面。
 *
 * 输入不经过框架: 宿主在主线程按每一帧的合成几何(可见区域、z 序、盖在其上的 momo 内容)同步决定原生事件的
 * 接收方——落在原生视图上(且没被 momo 内容盖住)的鼠标、滚动、手势直接交给它, 对 momo 而言指针离开了内容
 * (embedder/macos/mac_window.mm"平台视图输入")。本对象只在命中测试里占位(hitTestSelf), 让几何已变化、
 * 新帧尚未呈现时落到这里的事件不穿透到下面的 momo 内容。
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
  /** 平台视图自己接收原生输入, 不是盖在它上面的 momo 内容: 不记录可命中区域。 */
  override paintHitRegion(): void {}

  override paint(context: PaintingContext, offset: Offset): void {
    if (this.size.width <= 0 || this.size.height <= 0) return;
    context.addLayer(new PlatformViewLayer(Rect.fromLTWH(offset.dx, offset.dy, this.size.width, this.size.height), this.#viewId));
  }
}
