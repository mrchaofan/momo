// 无边框窗口(隐藏系统标题栏, 保留红绿灯)与拖动区域。
//   WindowChrome: 把所在窗口设为无边框外观;trafficLightsCenterY 让红绿灯与应用自绘的顶栏垂直居中。
//   WindowDragArea: 声明可拖动窗口的区域(无子节点时填满约束);每帧结束后按窗口汇总成矩形交给宿主。
//     宿主在这些区域里接管鼠标: 拖动移动窗口, 双击缩放(遵循系统设置);momo 不会收到这些点击。
import type { View as UiView } from "momo:ui";
import type { Key } from "../foundation/key";
import { Offset, Size } from "../painting/geometry";
import { RenderProxyBox } from "../rendering/box";
import type { BoxConstraints } from "../rendering/box_constraints";
import type { PipelineOwner } from "../rendering/object";
import { State, StatefulWidget, SingleChildRenderObjectWidget, type BuildContext, type Widget } from "./framework";
import { ViewScope } from "./view_scope";

/** 把所在窗口设为无边框外观(内容铺满窗口, 保留红绿灯)。 */
export class WindowChrome extends StatefulWidget {
  readonly hiddenTitleBar: boolean;
  readonly trafficLightsCenterY: number | null;
  readonly child: Widget;
  constructor(o: { key?: Key | null; hiddenTitleBar?: boolean; trafficLightsCenterY?: number | null; child: Widget }) {
    super(o.key);
    this.hiddenTitleBar = o.hiddenTitleBar ?? true;
    this.trafficLightsCenterY = o.trafficLightsCenterY ?? null;
    this.child = o.child;
  }
  createState(): State {
    return new WindowChromeState();
  }
}

class WindowChromeState extends State<WindowChrome> {
  #applied: string | null = null;
  #apply(): void {
    const view = ViewScope.maybeOf(this.context);
    if (view === null) return;
    const w = this.widget;
    const key = `${view.id}:${w.hiddenTitleBar}:${w.trafficLightsCenterY}`;
    if (key === this.#applied) return;
    this.#applied = key;
    view.setTitleBarStyle(w.hiddenTitleBar ? "hidden" : "default", w.trafficLightsCenterY ?? undefined);
  }
  override didChangeDependencies(): void {
    this.#apply();
  }
  override didUpdateWidget(): void {
    this.#apply();
  }
  build(_c: BuildContext): Widget {
    return this.widget.child;
  }
}

// ---------------------------------------------------------------------------

/** 当前挂在渲染树上的拖动区域。 */
const dragAreas = new Set<RenderWindowDragArea>();

export class RenderWindowDragArea extends RenderProxyBox {
  override attach(owner: PipelineOwner): void {
    super.attach(owner);
    dragAreas.add(this);
  }
  override detach(): void {
    dragAreas.delete(this);
    super.detach();
  }
  protected override computeSizeForNoChild(constraints: BoxConstraints): Size {
    return constraints.biggest;
  }
  override hitTestSelf(): boolean {
    return true;  // 吞掉 momo 侧的命中(宿主已接管该区域)
  }
}

/** 可拖动窗口的区域。无子节点时填满约束(常用于顶栏的空白处: Expanded(WindowDragArea()))。 */
export class WindowDragArea extends SingleChildRenderObjectWidget {
  constructor(o: { key?: Key | null; child?: Widget | null } = {}) {
    super(o);
  }
  createRenderObject(): RenderWindowDragArea {
    return new RenderWindowDragArea();
  }
}

/**
 * @internal 帧尾由 WidgetsBinding 调用: 按窗口汇总拖动区域(逻辑像素, 窗口左上原点), 有变化时交给宿主。
 * rootOf 把渲染树根映射到其窗口。
 */
export function flushDragRegions(viewOfRoot: (root: unknown) => UiView | null, last: Map<UiView, string>): void {
  const byView = new Map<UiView, number[]>();
  for (const area of dragAreas) {
    if (!area.hasSize) continue;
    let root: unknown = area;
    for (let p = area.parent; p !== null; p = p.parent) root = p;
    const view = viewOfRoot(root);
    if (view === null) continue;
    const o = area.localToGlobal(Offset.zero);
    const list = byView.get(view) ?? [];
    list.push(o.dx, o.dy, area.size.width, area.size.height);
    byView.set(view, list);
  }
  for (const view of new Set([...last.keys(), ...byView.keys()])) {
    const rects = byView.get(view) ?? [];
    const key = rects.map((v) => Math.round(v * 10) / 10).join(",");
    if (last.get(view) === key) continue;
    last.set(view, key);
    try {
      view.setDragRegions(rects);
    } catch {
      last.delete(view);  // 窗口已关闭
    }
  }
}
