// 无边框窗口(隐藏系统标题栏, 保留红绿灯)、缩放行为与拖动区域。
//   WindowChrome: 把所在窗口设为无边框外观;trafficLightsCenterY 让红绿灯与应用自绘的顶栏垂直居中;
//     resizable / resizeBorder / liveResizeFrameWait 配置窗口的缩放行为(见 WindowChrome 的说明)。
//   WindowDragArea: 声明可拖动窗口的区域(无子节点时填满约束);每帧结束后按窗口汇总成矩形交给宿主。
//     宿主在这些区域里接管鼠标: 拖动移动窗口, 双击缩放(遵循系统设置);momo 不会收到这些点击。
import type { View as UiView } from "momo:ui";
import type { Key } from "../foundation/key";
import { EdgeInsets } from "../painting/edge_insets";
import { Offset, Size } from "../painting/geometry";
import { RenderProxyBox } from "../rendering/box";
import type { BoxConstraints } from "../rendering/box_constraints";
import type { PipelineOwner } from "../rendering/object";
import { State, StatefulWidget, SingleChildRenderObjectWidget, type BuildContext, type Widget } from "./framework";
import { ViewScope } from "./view_scope";

/** WindowChrome 的缩放默认值(与宿主一致)。 */
export const kDefaultResizeBorder = 5;
export const kDefaultLiveResizeFrameWait = 100;

/**
 * 所在窗口的外观与缩放行为。
 *
 * 外观: hiddenTitleBar(缺省 true)隐藏系统标题栏、内容铺满窗口、保留红绿灯;trafficLightsCenterY 让红绿灯与
 * 应用自绘的顶栏垂直居中。
 *
 * 缩放(任一项给出时生效, 未给出的项取缺省值;都不给出时不改动窗口——保持 Window 打开时的 resizable):
 *   - resizable(true): 用户能否拖动边框缩放窗口。
 *   - resizeBorder(5): 窗口内侧的系统缩放边框(逻辑像素, 数字或各边独立的 EdgeInsets)。在这里光标显示为缩放形状、
 *     按下交给系统缩放, 优先于 WindowDragArea 与应用内容。0 = 该边没有内侧边框, 应用内容收到这里的全部输入
 *     (系统在窗口外侧的缩放区仍可用)——例如顶栏铺满可点击的标签页时用 EdgeInsets.only({ left: 5, right: 5, bottom: 5 })。
 *   - liveResizeFrameWait(100): 拖拽缩放时等待一帧新尺寸内容的上限(毫秒)。应用在此之内出帧则缩放无拉伸;
 *     0 = 不等待(松手前内容按旧帧拉伸, 窗口拖动最跟手)。
 */
export class WindowChrome extends StatefulWidget {
  readonly hiddenTitleBar: boolean;
  readonly trafficLightsCenterY: number | null;
  readonly resizable: boolean | null;
  readonly resizeBorder: EdgeInsets | number | null;
  readonly liveResizeFrameWait: number | null;
  readonly child: Widget;
  constructor(o: {
    key?: Key | null;
    hiddenTitleBar?: boolean;
    trafficLightsCenterY?: number | null;
    resizable?: boolean | null;
    resizeBorder?: EdgeInsets | number | null;
    liveResizeFrameWait?: number | null;
    child: Widget;
  }) {
    super(o.key);
    this.hiddenTitleBar = o.hiddenTitleBar ?? true;
    this.trafficLightsCenterY = o.trafficLightsCenterY ?? null;
    this.resizable = o.resizable ?? null;
    this.resizeBorder = o.resizeBorder ?? null;
    this.liveResizeFrameWait = o.liveResizeFrameWait ?? null;
    this.child = o.child;
  }
  createState(): State {
    return new WindowChromeState();
  }
}

class WindowChromeState extends State<WindowChrome> {
  #applied: string | null = null;
  #appliedResize: string | null = null;
  #apply(): void {
    const view = ViewScope.maybeOf(this.context);
    if (view === null) return;
    const w = this.widget;
    const key = `${view.id}:${w.hiddenTitleBar}:${w.trafficLightsCenterY}`;
    if (key !== this.#applied) {
      this.#applied = key;
      view.setTitleBarStyle(w.hiddenTitleBar ? "hidden" : "default", w.trafficLightsCenterY ?? undefined);
    }
    if (w.resizable === null && w.resizeBorder === null && w.liveResizeFrameWait === null) return;
    const border = w.resizeBorder ?? kDefaultResizeBorder;
    const options = {
      resizable: w.resizable ?? true,
      border: typeof border === "number"
        ? border
        : { left: border.left, top: border.top, right: border.right, bottom: border.bottom },
      liveResizeFrameWait: w.liveResizeFrameWait ?? kDefaultLiveResizeFrameWait,
    };
    const resizeKey = `${view.id}:${JSON.stringify(options)}`;
    if (resizeKey === this.#appliedResize) return;
    this.#appliedResize = resizeKey;
    view.setResizeOptions(options);
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
