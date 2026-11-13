// 多视图(多窗口): Window 声明式开关窗口、各窗口独立 MediaQuery/Overlay/渲染、共享状态、GlobalKey 跨窗口迁移。
import {
  Builder, Center, ColoredBox, Colors, Focus, FocusManager, FocusNode, GestureDetector, GlobalKey, MediaQuery, Offset,
  SizedBox, State, StatefulWidget, Text, ViewAnchor, Window, WindowChrome,
  WidgetsBinding, WindowService, EdgeInsets, GestureBinding, runWidget, type BuildContext, type Rect, type Widget,
} from "../packages/momo/src/index";
import { Phase } from "./tester";
import { views } from "momo:ui";

import { expect, group } from "./harness";
import { testWidgets } from "./util";

/** 带内部状态的计数器: 用来验证跨窗口迁移时 State 被保留。 */
class Counter extends StatefulWidget {
  constructor(o: { key?: GlobalKey }) {
    super(o.key);
  }
  createState(): State {
    return new CounterState();
  }
}
class CounterState extends State<Counter> {
  count = 0;
  build(_c: BuildContext): Widget {
    return new Text(`count ${this.count}`);
  }
}

group("multi-view", () => {
  testWidgets("Window opens a native window, renders into it and closes when removed", async (t, ctx) => {
    const mock = ctx.host.mock!;
    let sizes: string[] = [];
    const app = (open: boolean, title = "second"): Widget => new ViewAnchor({
      child: new Center({ child: new Text("main") }),
      views: open ? [new Window({
        title,
        width: 300,
        height: 200,
        child: new Builder({ builder: (c) => {
          sizes.push(MediaQuery.of(c).size.toString());
          return new Center({ child: new Text("in window") });
        } }),
      })] : [],
    });
    await t.pumpWidget(app(true));
    await t.pump();
    await t.pump();
    expect.equal(mock.openViews().length, 1);
    const id = mock.openViews()[0]!;
    expect.equal(mock.titles.get(id), "second");
    // 窗口内容渲染进自己的视图, 不出现在主窗口的场景里。
    const v1 = mock.sceneOpsByView.get(id) ?? [];
    expect.isTrue(v1.some((o) => o.op === "addPicture"), "view 1 got a scene");
    expect.isTrue(sizes.at(-1)!.includes("300") && sizes.at(-1)!.includes("200"), `MediaQuery is per window: ${sizes.at(-1)}`);
    expect.equal(t.byText("in window").length, 1);

    // 标题跟随 widget。
    await t.pumpWidget(app(true, "renamed"));
    await t.pump();
    expect.equal(mock.titles.get(id), "renamed");

    // 从状态中移除 → 关闭窗口。
    sizes = [];
    await t.pumpWidget(app(false));
    await t.pump();
    expect.deepEqual(mock.openViews(), []);
    expect.equal(WidgetsBinding.instance.viewHosts.size, 0);
  }, { mockOnly: true });

  // 标题栏: 应用只声明自己的标题栏高度;系统控制按钮占用的区域由宿主报告, 经 MediaQuery.windowControls 提供。
  testWidgets("WindowChrome hides the title bar and exposes the window controls area", async (t, ctx) => {
    const mock = ctx.host.mock!;
    let controls: Rect | null = null;
    const app = (hidden: boolean): Widget => new WindowChrome({
      hiddenTitleBar: hidden,
      titleBarHeight: 40,
      child: new Builder({ builder: (c) => {
        controls = MediaQuery.of(c).windowControls;
        return new SizedBox({});
      } }),
    });
    await t.pumpWidget(app(true));
    await t.pump();
    await t.pump();
    expect.deepEqual(mock.titleBars.get(0), { hidden: true, height: 40 });
    expect.isTrue(controls !== null, "the host reported the window controls");
    expect.equal(controls!.right, 78);
    expect.equal(controls!.bottom, 40);
    await t.pumpWidget(app(false));
    await t.pump();
    await t.pump();
    expect.equal(controls, null);
  }, { mockOnly: true });

  // 缩放边框只改边框;可否缩放是窗口属性(Window.resizable)。两者在宿主里合并, 互不覆盖;相同配置不重复下发。
  testWidgets("WindowChrome sets the resize border; Window sets resizable", async (t, ctx) => {
    const mock = ctx.host.mock!;
    mock.resizeBorders.clear();
    await t.pumpWidget(new WindowChrome({ child: new SizedBox({}) }));
    await t.pump();
    expect.isFalse(mock.resizeBorders.has(0), "no border given: the window keeps its own behaviour");
    await t.pumpWidget(new WindowChrome({ resizeBorder: EdgeInsets.only({ left: 4, right: 4, bottom: 4 }), child: new SizedBox({}) }));
    await t.pump();
    expect.deepEqual(mock.resizeBorders.get(0), { left: 4, top: 0, right: 4, bottom: 4 });

    const app = (resizable: boolean): Widget => new ViewAnchor({
      child: new SizedBox({}),
      views: [new Window({ key: null, width: 200, height: 100, resizable, child: new SizedBox({}) })],
    });
    await t.pumpWidget(app(false));
    await t.pump();
    expect.equal(mock.lastCreateViewOptions?.resizable, false, "create-time value");
    const id = mock.openViews()[0]!;
    await t.pumpWidget(app(true));
    await t.pump();
    expect.equal(mock.resizable.get(id), true, "updated with the widget");
    await t.pumpWidget(new SizedBox({}));
    await t.pump();
  }, { mockOnly: true });

  testWidgets("closing via the window's close button asks onCloseRequested and reports onClosed", async (t, ctx) => {
    const mock = ctx.host.mock!;
    let allow = false;
    let closed = 0;
    let open = true;
    const app = (): Widget => new ViewAnchor({
      child: new SizedBox({}),
      views: open ? [new Window({
        width: 200, height: 100,
        onCloseRequested: () => allow,
        onClosed: () => { closed++; open = false; },
        child: new Text("w"),
      })] : [],
    });
    await t.pumpWidget(app());
    await t.pump();
    const id = mock.openViews()[0]!;
    expect.isFalse(await mock.requestCloseView(id), "onCloseRequested returned false");
    expect.deepEqual(mock.openViews(), [id]);
    allow = true;
    expect.isTrue(await mock.requestCloseView(id));
    await t.pump();
    expect.equal(closed, 1);
    await t.pumpWidget(app());  // 应用按 onClosed 移除窗口
    await t.pump();
    expect.deepEqual(mock.openViews(), []);
  }, { mockOnly: true });

  testWidgets("GlobalKey moves a subtree between windows keeping its State", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const key = new GlobalKey<CounterState>();
    const app = (inWindow: boolean): Widget => new ViewAnchor({
      child: inWindow ? new SizedBox({}) : new Counter({ key }),
      views: [new Window({ width: 200, height: 100, child: inWindow ? new Counter({ key }) : new ColoredBox({ color: Colors.white }) })],
    });
    await t.pumpWidget(app(false));
    await t.pump();
    key.currentState!.count = 7;
    key.currentState!.setState();
    await t.pump();
    expect.equal(t.byText("count 7").length, 1);
    const state = key.currentState;
    await t.pumpWidget(app(true));
    await t.pump();
    expect.equal(key.currentState, state, "same State object after moving windows");
    expect.equal(t.byText("count 7").length, 1);
    expect.equal(mock.openViews().length, 1);
    expect.isTrue((mock.sceneOpsByView.get(mock.openViews()[0]!) ?? []).some((o) => o.op === "addPicture"));
  }, { mockOnly: true });

  testWidgets("each window has its own focus scope; window focus restores it", async (t, ctx) => {
    const mainNode = new FocusNode({ debugLabel: "main" });
    const windowNode = new FocusNode({ debugLabel: "window" });
    await t.pumpWidget(new ViewAnchor({
      child: new Focus({ focusNode: mainNode, child: new SizedBox({}) }),
      views: [new Window({ width: 200, height: 100, child: new Focus({ focusNode: windowNode, child: new SizedBox({}) }) })],
    }));
    await t.pump();
    windowNode.requestFocus();
    await t.pump();
    expect.equal(FocusManager.instance.primaryFocus, windowNode);
    // 窗口的作用域不在主窗口作用域之下。
    expect.isFalse(mainNode.enclosingScope!.hasFocus, "main scope does not contain the window's focus");
    mainNode.requestFocus();
    await t.pump();
    // 窗口 1 重新获得系统焦点: 恢复它上次的焦点。
    const view = views.get(ctx.host.mock!.openViews()[0]!)!;
    view.onFocusChanged?.(true);
    await t.pump();
    expect.equal(FocusManager.instance.primaryFocus, windowNode);
  }, { mockOnly: true });

  // 所有窗口对等: 隐式视图(runApp 渲染的窗口)关闭后, 根照常构建, 其他窗口照常渲染;根渲染树不再提交。
  testWidgets("the implicit view can be closed while other windows keep rendering", async (t, ctx) => {
    const mock = ctx.host.mock!;
    let builds = 0;
    const app = (): Widget => new ViewAnchor({
      child: new Text("root"),
      views: [new Window({ width: 200, height: 100, child: new Builder({ builder: () => { builds++; return new Text("other"); } }) })],
    });
    await t.pumpWidget(app());
    await t.pump();
    const other = mock.openViews()[0]!;
    // 用户关闭了启动窗口(JS 未拦截)。
    expect.isTrue(await mock.requestCloseView(0));
    await t.flushMicrotasks();
    expect.isFalse(mock.hasView(0));
    const rendered = mock.renderedScenes;
    const before = builds;
    await t.pumpWidget(app());
    await t.pump();
    expect.equal(mock.renderedScenes, rendered, "nothing is submitted to the closed implicit view");
    expect.isTrue(builds > before || (mock.sceneOpsByView.get(other) ?? []).length > 0, "the other window keeps rendering");
    expect.deepEqual(mock.openViews(), [other]);
  }, { mockOnly: true });

  // runWidget: 根不绑定任何视图;启动窗口经 Window({ view }) 接管, 与新开的窗口一样声明属性、卸载即关闭。
  testWidgets("runWidget adopts the startup view through Window({ view })", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const startup = views.get(0)!;
    let closed = 0;
    const app = (open: boolean): Widget => new ViewAnchor({
      views: open ? [new Window({ view: startup, title: "adopted", onClosed: () => closed++, child: new Text("in startup window") })] : [],
    });
    runWidget(app(true));
    await t.pump();
    await t.pump();
    expect.equal(mock.titles.get(0), "adopted");
    expect.isTrue((mock.sceneOpsByView.get(0) ?? []).some((o) => o.op === "addPicture"), "content renders into view 0");
    expect.equal(WidgetsBinding.instance.viewHosts.get(0)?.view, startup);
    // 指针命中走接管它的 View。
    let taps = 0;
    runWidget(new ViewAnchor({ views: [new Window({ view: startup, child: new GestureDetector({ onTap: () => taps++, child: new ColoredBox({ color: Colors.white }) }) })] }));
    await t.pump();
    await t.tap(new Offset(10, 10));
    expect.equal(taps, 1);
    // 卸载即关闭(启动窗口与其他窗口没有区别)。
    runWidget(app(false));
    await t.pump();
    await t.flushMicrotasks();
    expect.isFalse(mock.hasView(0), "unmounting the Window closes the startup view");
    expect.equal(closed, 0, "closed by the app, not by the host");
  }, { mockOnly: true });

  // 退出应用(Cmd+Q)与关闭窗口是两件事: 前者询问 didRequestAppExit。
  testWidgets("app exit requests ask didRequestAppExit", async (t, ctx) => {
    const mock = ctx.host.mock!;
    await t.pumpWidget(new SizedBox({}));
    let allow = false;
    const observer = { didRequestAppExit: () => allow };
    WidgetsBinding.instance.addObserver(observer);
    try {
      expect.isFalse(await mock.requestAppExit());
      allow = true;
      expect.isTrue(await mock.requestAppExit());
    } finally {
      WidgetsBinding.instance.removeObserver(observer);
    }
  }, { mockOnly: true });

  testWidgets("WindowService caches frames and follows host updates", async (t, ctx) => {
    const mock = ctx.host.mock!;
    await t.pumpWidget(new SizedBox({}));
    await WindowService.instance.refresh(0);
    expect.deepEqual(WindowService.instance.frameOf(0), mock.frames.get(0));
    WindowService.instance.setPosition(0, 30, 40);
    await t.flushMicrotasks();
    expect.equal(WindowService.instance.frameOf(0)?.x, 30);
    expect.isTrue(WindowService.instance.capabilitiesOf(0)?.has("moveLoop") ?? false);
  }, { mockOnly: true });

  // 拖动中并入另一个窗口(浏览器合并标签): 触发拖动的窗口被关闭, 捕获的序列照常交给捕获者;
  // 只有原生接收窗口本身被关闭时, 序列才以取消结束。
  testWidgets("a captured drag survives closing the window it started in", async (t, ctx) => {
    const mock = ctx.host.mock!;
    let open = true;
    let source = -1;
    const app = (): Widget => new ViewAnchor({
      child: new ColoredBox({ color: Colors.white }),
      views: open ? [new Window({ width: 200, height: 100, onOpened: (v) => { source = v.id; }, child: new ColoredBox({ color: Colors.white }) })] : [],
    });
    await t.pumpWidget(app());
    await t.pump();
    const routed: string[] = [];
    t.sendPointer(Phase.down, new Offset(20, 20), { view: source });
    const capture = GestureBinding.instance.capturePointer((e) => routed.push(`${e.constructor.name}@${e.viewId}`))!;
    capture.moveToView(0);  // 并入视图 0 的窗口: 原生接收窗口改交
    await t.flushMicrotasks();
    expect.deepEqual(mock.captures.at(-1), 0);
    open = false;  // 源窗口被并走、关闭
    await t.pumpWidget(app());
    await t.pump();
    expect.isFalse(mock.hasView(source));
    t.sendPointer(Phase.move, new Offset(300, 20), { view: source });  // 宿主改投视图 0(坐标按屏幕换算)
    t.sendPointer(Phase.up, new Offset(300, 20), { view: source, buttons: 0 });
    expect.deepEqual(routed, ["PointerMoveEvent@0", "PointerUpEvent@0"], "the drag continues to the release");
  }, { mockOnly: true });

  testWidgets("closing the receiving window cancels the captured sequence", async (t, ctx) => {
    const mock = ctx.host.mock!;
    let target = -1;
    await t.pumpWidget(new ViewAnchor({
      child: new ColoredBox({ color: Colors.white }),
      views: [new Window({ width: 200, height: 100, onOpened: (v) => { target = v.id; }, child: new SizedBox({}) })],
    }));
    await t.pump();
    const routed: string[] = [];
    t.sendPointer(Phase.down, new Offset(20, 20));
    const capture = GestureBinding.instance.capturePointer((e) => routed.push(e.constructor.name))!;
    capture.moveToView(target);
    await t.flushMicrotasks();
    expect.isTrue(await mock.requestCloseView(target));
    expect.deepEqual(routed, ["PointerCancelEvent"]);
    expect.isFalse(capture.active);
  }, { mockOnly: true });

  testWidgets("createView failure reports onCreateError and renders nothing", async (t, ctx) => {
    const mock = ctx.host.mock!;
    mock.failNextCreateView = true;
    let error: unknown = null;
    await t.pumpWidget(new ViewAnchor({
      child: new SizedBox({}),
      views: [new Window({ width: 10, height: 10, onCreateError: (e) => { error = e; }, child: new Text("never") })],
    }));
    await t.pump();
    expect.isTrue(error instanceof Error);
    expect.equal(t.byText("never").length, 0);
    expect.deepEqual(mock.openViews(), []);
  }, { mockOnly: true });
});
