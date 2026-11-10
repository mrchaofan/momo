import { GestureBinding } from "../gestures/binding";
import {
  PointerDownEvent, PointerGestureEvent, PointerHoverEvent, PointerMoveEvent, PointerPressureEvent, PointerScrollEvent,
  PointerUpEvent, type PointerEnterEvent, type PointerEvent, type PointerExitEvent,
} from "../gestures/events";
import type { HitTestEntry } from "../gestures/hit_test";
import { Offset, Rect, Size } from "../painting/geometry";
import { RenderBox } from "./box";
import { BoxConstraints } from "./box_constraints";
import { PlatformViewLayer } from "./layer";
import { kMouseTrackerAnnotation, type MouseCursor, type MouseTrackerAnnotation } from "./mouse_tracker";
import type { PaintingContext } from "./object";

/**
 * 平台视图(扩展创建的系统原生视图, 如 WKWebView)在渲染树中的占位与指针接收者(docs/05 §9)。
 * 尺寸取约束的最大值;绘制时追加一个 PlatformViewLayer, 引擎合成时把原生视图摆到这个位置,
 * 之后绘制的 momo 内容叠在它上面。
 *
 * 输入: 原生视图不按几何直接收事件, 由框架决定。本对象像普通 render object 一样参与命中测试:
 *   - 命中路径上的按下/移动/抬起/悬停经 {@link GestureBinding.forwardToPlatformView} 交给宿主, 宿主把
 *     对应的原始事件投递给原生视图。按下序列的后续事件沿按下时的命中路径到达(框架的隐式捕获),
 *     原生视图内部由宿主在按下时捕获接收子视图。
 *   - 滚动与平台手势(捏合/旋转/轻扫/智能缩放/查询…): 先于外层 Scrollable 等在 PointerSignalResolver 上登记
 *     (命中路径由内到外), 只归原生视图;触控板手势由 GestureBinding 锁定在开始时的命中结果上。
 *   - Force Touch 压力: 属于按下序列, 沿按下时的命中路径到达(与拖动相同)。
 *   - 悬停归属: 作为 MouseTrackerAnnotation, 进入/离开时通知宿主(带触发它的原生事件 id)。进入/离开与
 *     转发的事件走同一条有序通道, 宿主据此推导原生视图的跟踪区状态(:hover 等), 不使用 AppKit 自己
 *     同步产生的进入/离开(与经 ui 线程往返的事件没有先后关系)。光标 "defer", 指针归原生视图时由它管理。
 * 盖在它上面且可命中的 momo 内容自然先于它命中——命中测试即遮挡判断。
 */
export class RenderPlatformView extends RenderBox implements MouseTrackerAnnotation {
  readonly [kMouseTrackerAnnotation] = true as const;
  readonly cursor: MouseCursor = "defer";
  readonly onEnter = (event: PointerEnterEvent): void => this.#setHovered(true, event.nativeEventId);
  readonly onExit = (event: PointerExitEvent): void => this.#setHovered(false, event.nativeEventId);
  #viewId: number;
  #hovered = false;

  constructor(viewId: number) {
    super();
    this.#viewId = viewId;
  }

  get viewId(): number {
    return this.#viewId;
  }
  set viewId(v: number) {
    if (v !== this.#viewId) {
      this.#setHovered(false);
      this.#viewId = v;
      this.markNeedsPaint();
    }
  }

  get validForMouseTracker(): boolean {
    return this.attached;
  }

  #setHovered(hovered: boolean, nativeEventId = 0): void {
    if (hovered === this.#hovered) return;
    this.#hovered = hovered;
    GestureBinding.instance.setPlatformViewHover(this.#viewId, hovered, nativeEventId);
  }

  override detach(): void {
    // 移出树的区域不再收到 MouseTracker 的离开(与 Flutter 一致): 自行交还指针归属。
    this.#setHovered(false);
    super.detach();
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

  override handleEvent(event: PointerEvent, _entry: HitTestEntry): void {
    const binding = GestureBinding.instance;
    if (event instanceof PointerScrollEvent || event instanceof PointerGestureEvent) {
      const viewId = this.#viewId;
      binding.pointerSignalResolver.register(event, (e) => binding.forwardToPlatformView(viewId, e));
      return;
    }
    if (event instanceof PointerDownEvent || event instanceof PointerMoveEvent || event instanceof PointerUpEvent ||
        event instanceof PointerHoverEvent || event instanceof PointerPressureEvent) {
      binding.forwardToPlatformView(this.#viewId, event);
    }
  }

  override paint(context: PaintingContext, offset: Offset): void {
    if (this.size.width <= 0 || this.size.height <= 0) return;
    context.addLayer(new PlatformViewLayer(Rect.fromLTWH(offset.dx, offset.dy, this.size.width, this.size.height), this.#viewId));
  }
}
