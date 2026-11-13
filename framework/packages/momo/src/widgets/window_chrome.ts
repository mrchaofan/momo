// 自绘窗口外观: 隐藏系统标题栏与缩放行为。
//   WindowChrome: 所在窗口的外观与缩放边框(声明式;经宿主的 momo/window 通道生效, 不经引擎)。
//     隐藏标题栏后系统窗口控制按钮浮在内容之上, 占用区域由宿主报告(MediaQuery.windowControls), 应用据此
//     避让——框架与应用都不写死平台尺寸。
// 拖动窗口: 框架只提供能力(WindowService.startDrag(view, sequenceId), 以进行中的按下序列开始系统拖动),
//   哪里可以拖动由应用自己决定(例如在按下时调用;见 examples/webview 的 WindowDragArea)。
import type { Key } from "../foundation/key";
import { EdgeInsets } from "../painting/edge_insets";
import { WindowService } from "../services/window";
import { State, StatefulWidget, type BuildContext, type Widget } from "./framework";
import { ViewScope } from "./view_scope";

/**
 * 所在窗口的外观(声明式, 随 widget 更新;窗口的属性如标题、可否缩放在 Window 上声明)。
 *
 * - hiddenTitleBar(缺省 true): 隐藏系统标题栏、内容铺满窗口、保留系统窗口控制按钮;titleBarHeight 是应用自绘
 *   标题栏的高度——宿主让控制按钮在这个高度内垂直居中。控制按钮占用的区域见 MediaQuery.windowControls。
 * - resizeBorder: 窗口内侧的系统缩放边框(逻辑像素, 数字或各边独立的 EdgeInsets;未给出时用宿主默认 5)。在这里
 *   光标显示为缩放形状、按下交给系统缩放, 优先于应用内容。0 = 该边没有内侧边框, 应用内容收到这里的全部输入
 *   (系统在窗口外侧的缩放区仍可用)——例如顶栏铺满可点击的标签页时用 EdgeInsets.only({ left: 5, right: 5, bottom: 5 })。
 *
 * 平台不支持某项(如隐藏标题栏)时该项不生效, windowControls 保持 null。
 */
export class WindowChrome extends StatefulWidget {
  readonly hiddenTitleBar: boolean;
  readonly titleBarHeight: number | null;
  readonly resizeBorder: EdgeInsets | number | null;
  readonly child: Widget;
  constructor(o: {
    key?: Key | null;
    hiddenTitleBar?: boolean;
    titleBarHeight?: number | null;
    resizeBorder?: EdgeInsets | number | null;
    child: Widget;
  }) {
    super(o.key);
    this.hiddenTitleBar = o.hiddenTitleBar ?? true;
    this.titleBarHeight = o.titleBarHeight ?? null;
    this.resizeBorder = o.resizeBorder ?? null;
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
    const windows = WindowService.instance;
    const key = `${view.id}:${w.hiddenTitleBar}:${w.titleBarHeight}`;
    if (key !== this.#applied) {
      this.#applied = key;
      windows.setTitleBar(view.id, { hidden: w.hiddenTitleBar, height: w.titleBarHeight ?? undefined });
    }
    const border = w.resizeBorder;
    if (border === null) return;
    const edges = typeof border === "number" ? border : { left: border.left, top: border.top, right: border.right, bottom: border.bottom };
    const resizeKey = `${view.id}:${JSON.stringify(edges)}`;
    if (resizeKey === this.#appliedResize) return;
    this.#appliedResize = resizeKey;
    windows.setResizeBorder(view.id, edges);
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
