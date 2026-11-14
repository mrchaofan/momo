// 多窗口: Widget(原生窗口对象)的方法与事件、RootView 把内容渲染进窗口、各窗口独立 MediaQuery/Overlay/渲染、
// 共享状态、GlobalKey 跨窗口迁移;app 的退出流程与窗口全部关闭。
import {
  Builder, Center, ColoredBox, Colors, Focus, FocusManager, FocusNode, GestureDetector, GlobalKey, MediaQuery, Offset,
  RootView, SizedBox, State, StatefulView, Text, ViewAnchor, Widget, WindowChrome, app,
  ViewsBinding, EdgeInsets, GestureBinding, runView, type BuildContext, type Rect, type View,
} from "../packages/momo/src/index";
import { Phase } from "./tester";

import { expect, group } from "./harness";
import { testViews } from "./util";

/** 带内部状态的计数器: 用来验证跨窗口迁移时 State 被保留。 */
class Counter extends StatefulView {
  constructor(o: { key?: GlobalKey }) {
    super(o.key);
  }
  createState(): State {
    return new CounterState();
  }
}
class CounterState extends State<Counter> {
  count = 0;
  build(_c: BuildContext): View {
    return new Text(`count ${this.count}`);
  }
}

/** 主窗口内容 + 其他窗口(各自一个 RootView)。 */
function withWindows(child: View, windows: readonly { widget: Widget; child: View }[]): View {
  return new ViewAnchor({ child, views: windows.map((w) => new RootView({ widget: w.widget, child: w.child })) });
}

group("multi-window", () => {
  testViews("Widget.create opens a native window; RootView renders into it", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const sizes: string[] = [];
    const win = await Widget.create({ title: "second", width: 300, height: 200 });
    expect.deepEqual(mock.openViews(), [win.id]);
    expect.equal(mock.titles.get(win.id), "second");
    expect.equal(Widget.fromId(win.id), win, "one object per window");
    expect.isTrue(win.viewport !== null);
    const content = new Builder({ builder: (c) => {
      sizes.push(MediaQuery.of(c).size.toString());
      return new Center({ child: new Text("in window") });
    } });
    await t.pumpView(withWindows(new Center({ child: new Text("main") }), [{ widget: win, child: content }]));
    await t.pump();
    // 窗口内容渲染进自己的视图, 不出现在主窗口的场景里。
    expect.isTrue((mock.sceneOpsByView.get(win.id) ?? []).some((o) => o.op === "addPicture"), "the window got a scene");
    expect.isTrue(sizes.at(-1)!.includes("300") && sizes.at(-1)!.includes("200"), `MediaQuery is per window: ${sizes.at(-1)}`);
    expect.equal(t.byText("in window").length, 1);

    win.setTitle("renamed");
    await t.flushMicrotasks();
    expect.equal(mock.titles.get(win.id), "renamed");

    // destroy: 立即关闭(不询问), 'closed' 后对象作废;应用把 RootView 移出树。
    let closed = 0;
    win.on("closed", () => closed++);
    win.destroy();
    await t.flushMicrotasks();
    expect.equal(closed, 1);
    expect.isTrue(win.isDestroyed());
    expect.equal(Widget.fromId(win.id), null);
    await t.pumpView(new Center({ child: new Text("main") }));
    await t.pump();
    expect.deepEqual(mock.openViews(), []);
    expect.equal(ViewsBinding.instance.viewHosts.size, 0);
  }, { mockOnly: true });

  // 标题栏: 应用只声明自己的标题栏高度;系统控制按钮占用的区域由宿主报告, 经 MediaQuery.windowControls 提供。
  testViews("WindowChrome hides the title bar and exposes the window controls area", async (t, ctx) => {
    const mock = ctx.host.mock!;
    let controls: Rect | null = null;
    const root = (hidden: boolean): View => new WindowChrome({
      hiddenTitleBar: hidden,
      titleBarHeight: 40,
      child: new Builder({ builder: (c) => {
        controls = MediaQuery.of(c).windowControls;
        return new SizedBox({});
      } }),
    });
    await t.pumpView(root(true));
    await t.pump();
    await t.pump();
    expect.deepEqual(mock.titleBars.get(0), { hidden: true, height: 40 });
    expect.isTrue(controls !== null, "the host reported the window controls");
    expect.equal(controls!.right, 78);
    expect.equal(controls!.bottom, 40);
    await t.pumpView(root(false));
    await t.pump();
    await t.pump();
    expect.equal(controls, null);
  }, { mockOnly: true });

  // 缩放边框由 WindowChrome 声明;可否缩放是窗口属性(Widget.setResizable)。两者在宿主里合并, 互不覆盖。
  testViews("WindowChrome sets the resize border; Widget sets resizable", async (t, ctx) => {
    const mock = ctx.host.mock!;
    mock.resizeBorders.clear();
    await t.pumpView(new WindowChrome({ child: new SizedBox({}) }));
    await t.pump();
    expect.isFalse(mock.resizeBorders.has(0), "no border given: the window keeps its own behaviour");
    await t.pumpView(new WindowChrome({ resizeBorder: EdgeInsets.only({ left: 4, right: 4, bottom: 4 }), child: new SizedBox({}) }));
    await t.pump();
    expect.deepEqual(mock.resizeBorders.get(0), { left: 4, top: 0, right: 4, bottom: 4 });
    await t.pumpView(new WindowChrome({ child: new SizedBox({}) }));
    await t.pump();
    expect.isFalse(mock.resizeBorders.has(0), "border removed: the window goes back to the host default");

    const win = await Widget.create({ width: 200, height: 100, resizable: false });
    expect.equal(mock.lastCreateViewOptions?.resizable, false, "create-time value");
    win.setResizable(true);
    await t.flushMicrotasks();
    expect.equal(mock.resizable.get(win.id), true);
  }, { mockOnly: true });

  testViews("the close button asks the widget's 'close' listeners, then emits 'closed'", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const win = await Widget.create({ width: 200, height: 100 });
    let allow = false;
    const events: string[] = [];
    win.on("close", (e) => { events.push("close"); if (!allow) e.preventDefault(); });
    win.on("closed", () => events.push("closed"));
    await t.pumpView(withWindows(new SizedBox({}), [{ widget: win, child: new Text("w") }]));
    await t.pump();
    expect.isFalse(await mock.requestCloseView(win.id), "'close' was prevented");
    expect.deepEqual(mock.openViews(), [win.id]);
    allow = true;
    expect.isTrue(await mock.requestCloseView(win.id));
    await t.flushMicrotasks();
    expect.deepEqual(events, ["close", "close", "closed"]);
    expect.isTrue(win.isDestroyed());
    // close(): 与关闭按钮相同(先询问)。
    const other = await Widget.create({ width: 200, height: 100 });
    other.on("close", (e) => e.preventDefault());
    other.close();
    await t.flushMicrotasks();
    expect.isFalse(other.isDestroyed(), "close() asks first");
  }, { mockOnly: true });

  testViews("GlobalKey moves a subtree between windows keeping its State", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const key = new GlobalKey<CounterState>();
    const win = await Widget.create({ width: 200, height: 100 });
    const root = (inWindow: boolean): View => withWindows(
      inWindow ? new SizedBox({}) : new Counter({ key }),
      [{ widget: win, child: inWindow ? new Counter({ key }) : new ColoredBox({ color: Colors.white }) }],
    );
    await t.pumpView(root(false));
    await t.pump();
    key.currentState!.count = 7;
    key.currentState!.setState();
    await t.pump();
    expect.equal(t.byText("count 7").length, 1);
    const state = key.currentState;
    await t.pumpView(root(true));
    await t.pump();
    expect.equal(key.currentState, state, "same State object after moving windows");
    expect.equal(t.byText("count 7").length, 1);
    expect.isTrue((mock.sceneOpsByView.get(win.id) ?? []).some((o) => o.op === "addPicture"));
  }, { mockOnly: true });

  testViews("each window has its own focus scope; window focus restores it", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const mainNode = new FocusNode({ debugLabel: "main" });
    const windowNode = new FocusNode({ debugLabel: "window" });
    const win = await Widget.create({ width: 200, height: 100 });
    const events: string[] = [];
    win.on("focus", () => events.push("focus"));
    win.on("blur", () => events.push("blur"));
    await t.pumpView(withWindows(new Focus({ focusNode: mainNode, child: new SizedBox({}) }),
      [{ widget: win, child: new Focus({ focusNode: windowNode, child: new SizedBox({}) }) }]));
    await t.pump();
    windowNode.requestFocus();
    await t.pump();
    expect.equal(FocusManager.instance.primaryFocus, windowNode);
    // 窗口的作用域不在主窗口作用域之下。
    expect.isFalse(mainNode.enclosingScope!.hasFocus, "main scope does not contain the window's focus");
    mainNode.requestFocus();
    await t.pump();
    // 窗口重新获得系统焦点: 恢复它上次的焦点;Widget 收到 'focus'。
    mock.setWindowFocus(win.id, true);
    await t.pump();
    expect.equal(FocusManager.instance.primaryFocus, windowNode);
    mock.setWindowFocus(win.id, false);
    await t.flushMicrotasks();
    expect.deepEqual(events, ["focus", "blur"]);
  }, { mockOnly: true });

  // 窗口状态: 最小化/最大化/还原/隐藏/显示 → 事件;不可见的窗口不提交画面。
  testViews("window state changes emit events and hidden windows are not drawn", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const win = await Widget.create({ width: 200, height: 100 });
    const events: string[] = [];
    for (const e of ["minimize", "maximize", "restore", "hide", "show"] as const) win.on(e, () => events.push(e));
    await t.pumpView(withWindows(new SizedBox({}), [{ widget: win, child: new ColoredBox({ color: Colors.white }) }]));
    await t.pump();
    win.minimize();
    await t.flushMicrotasks();
    expect.isTrue(win.isMinimized);
    expect.isFalse(win.viewport!.visible, "a minimized window's viewport is not visible");
    mock.sceneOpsByView.delete(win.id);
    t.binding.renderView.markNeedsPaint();
    await t.pump();
    expect.isFalse(mock.sceneOpsByView.has(win.id), "no frame for a minimized window");
    win.restore();
    win.maximize();
    win.restore();
    win.hide();
    win.show();
    await t.flushMicrotasks();
    expect.deepEqual(events, ["minimize", "restore", "maximize", "restore", "hide", "show"]);
    // 用户在系统里操作窗口: 同样的事件。
    mock.setWindowState(win.id, { minimized: true });
    await t.flushMicrotasks();
    expect.equal(events.at(-1), "minimize");
  }, { mockOnly: true });

  // show: false: 第一帧画好后 'ready-to-show', 应用再显示(避免先看到空白窗口)。
  testViews("a window created with show: false emits ready-to-show after its first frame", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const win = await Widget.create({ width: 200, height: 100, show: false });
    expect.isFalse(mock.windowStates.get(win.id)!.visible);
    let ready = 0;
    win.once("ready-to-show", () => { ready++; win.show(); });
    await t.pumpView(withWindows(new SizedBox({}), [{ widget: win, child: new ColoredBox({ color: Colors.white }) }]));
    await t.pump();
    await t.flushMicrotasks();
    expect.equal(ready, 1);
    expect.isTrue(mock.windowStates.get(win.id)!.visible);
  }, { mockOnly: true });

  // 所有窗口对等: 隐式视图(runApp 渲染的窗口)关闭后, 根照常构建, 其他窗口照常渲染;根渲染树不再提交。
  testViews("the implicit window can be closed while other windows keep rendering", async (t, ctx) => {
    const mock = ctx.host.mock!;
    let builds = 0;
    const win = await Widget.create({ width: 200, height: 100 });
    const root = (): View => withWindows(new Text("root"),
      [{ widget: win, child: new Builder({ builder: () => { builds++; return new Text("other"); } }) }]);
    await t.pumpView(root());
    await t.pump();
    expect.isTrue(await mock.requestCloseView(0));
    await t.flushMicrotasks();
    expect.isFalse(mock.hasView(0));
    const rendered = mock.renderedScenes;
    const before = builds;
    await t.pumpView(root());
    await t.pump();
    expect.equal(mock.renderedScenes, rendered, "nothing is submitted to the closed implicit window");
    expect.isTrue(builds > before || (mock.sceneOpsByView.get(win.id) ?? []).length > 0, "the other window keeps rendering");
    expect.deepEqual(mock.openViews(), [win.id]);
  }, { mockOnly: true });

  // runView: 根不绑定任何视图;启动窗口(Widget.fromId(0))与其他窗口一样由 RootView 渲染。
  testViews("runView renders the startup window through RootView", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const startup = Widget.fromId(0)!;
    startup.setTitle("adopted");
    runView(withWindows(new SizedBox({}), [{ widget: startup, child: new Text("in startup window") }]));
    await t.pump();
    await t.pump();
    expect.equal(mock.titles.get(0), "adopted");
    expect.isTrue((mock.sceneOpsByView.get(0) ?? []).some((o) => o.op === "addPicture"), "content renders into view 0");
    expect.equal(ViewsBinding.instance.viewHosts.get(0)?.viewport, startup.viewport);
    // 指针命中走渲染它的 RootView。
    let taps = 0;
    runView(withWindows(new SizedBox({}), [{
      widget: startup, child: new GestureDetector({ onTap: () => taps++, child: new ColoredBox({ color: Colors.white }) }),
    }]));
    await t.pump();
    await t.tap(new Offset(10, 10));
    expect.equal(taps, 1);
    startup.destroy();
    await t.flushMicrotasks();
    expect.isFalse(mock.hasView(0));
  }, { mockOnly: true });

  // 最后一个窗口关闭: 应用不退出, 只收到 'window-all-closed'。
  testViews("closing the last window keeps the app running", async (t, ctx) => {
    const mock = ctx.host.mock!;
    await t.pumpView(new SizedBox({}));
    let allClosed = 0;
    app.on("window-all-closed", () => allClosed++);
    expect.isTrue(await mock.requestCloseView(0));
    await t.flushMicrotasks();
    expect.equal(allClosed, 1);
    expect.equal(mock.exitCode, null, "the app keeps running");
    // 点击 Dock 图标: 'activate'(通常在此重新打开窗口)。
    let activated: boolean | null = null;
    app.on("activate", (info) => { activated = info.hasVisibleWindows; });
    mock.reopen(false);
    await t.flushMicrotasks();
    expect.equal(activated, false);
  }, { mockOnly: true });

  // 退出流程: before-quit → 每个窗口的 close → will-quit → 退出;任一步取消即中止。
  testViews("app quit asks before-quit, every window, then will-quit", async (t, ctx) => {
    const mock = ctx.host.mock!;
    await t.pumpView(new SizedBox({}));
    const win = await Widget.create({ width: 200, height: 100 });
    const log: string[] = [];
    let cancelBefore = true;
    let keepWindow = true;
    app.on("before-quit", (e) => { log.push("before-quit"); if (cancelBefore) e.preventDefault(); });
    win.on("close", (e) => { log.push("close"); if (keepWindow) e.preventDefault(); });
    app.on("will-quit", () => log.push("will-quit"));
    app.on("quit", () => log.push("quit"));

    expect.isFalse(await mock.requestQuit());
    expect.deepEqual(log, ["before-quit"]);
    cancelBefore = false;
    expect.isFalse(await mock.requestQuit(), "a window refused to close");
    expect.isFalse(win.isDestroyed());
    keepWindow = false;
    log.length = 0;
    expect.isTrue(await mock.requestQuit());
    await t.flushMicrotasks();
    expect.deepEqual(log, ["before-quit", "close", "will-quit", "quit"]);
    expect.equal(mock.exitCode, 0);
  }, { mockOnly: true });

  testViews("app.quit() and app.exit(code) reach the host", async (t, ctx) => {
    const mock = ctx.host.mock!;
    await t.pumpView(new SizedBox({}));
    app.on("before-quit", (e) => e.preventDefault());
    app.quit();
    await t.flushMicrotasks();
    expect.equal(mock.exitCode, null, "quit was cancelled");
    app.exit(3);
    await t.flushMicrotasks();
    expect.equal(mock.exitCode, 3, "exit does not ask");
    expect.deepEqual(mock.appCalls, ["quit", "exit"]);
  }, { mockOnly: true });

  testViews("Widget caches bounds and follows host updates", async (t, ctx) => {
    const mock = ctx.host.mock!;
    await t.pumpView(new SizedBox({}));
    const win = Widget.fromId(0)!;
    await win.refresh();
    expect.deepEqual(win.bounds, mock.frames.get(0));
    const moves: number[] = [];
    win.on("move", (b) => moves.push(b.x));
    win.setPosition(30, 40);
    await t.flushMicrotasks();
    expect.equal(win.bounds?.x, 30);
    expect.deepEqual(moves, [30]);
    expect.isTrue(win.capabilities?.has("moveLoop") ?? false);
  }, { mockOnly: true });

  // 拖动中并入另一个窗口(浏览器合并标签): 触发拖动的窗口被关闭, 捕获的序列照常交给捕获者;
  // 只有原生接收窗口本身被关闭时, 序列才以取消结束。
  testViews("a captured drag survives closing the window it started in", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const win = await Widget.create({ width: 200, height: 100 });
    await t.pumpView(withWindows(new ColoredBox({ color: Colors.white }), [{ widget: win, child: new ColoredBox({ color: Colors.white }) }]));
    await t.pump();
    const routed: string[] = [];
    t.sendPointer(Phase.down, new Offset(20, 20), { view: win.id });
    const capture = GestureBinding.instance.capturePointer((e) => routed.push(`${e.constructor.name}@${e.viewId}`))!;
    capture.moveToView(0);  // 并入视图 0 的窗口: 原生接收窗口改交
    await t.flushMicrotasks();
    expect.deepEqual(mock.captures.at(-1), 0);
    win.destroy();  // 源窗口被并走、关闭
    await t.flushMicrotasks();
    await t.pumpView(new ColoredBox({ color: Colors.white }));
    await t.pump();
    expect.isFalse(mock.hasView(win.id));
    t.sendPointer(Phase.move, new Offset(300, 20), { view: win.id });  // 宿主改投视图 0(坐标按屏幕换算)
    t.sendPointer(Phase.up, new Offset(300, 20), { view: win.id, buttons: 0 });
    expect.deepEqual(routed, ["PointerMoveEvent@0", "PointerUpEvent@0"], "the drag continues to the release");
  }, { mockOnly: true });

  testViews("closing the receiving window cancels the captured sequence", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const win = await Widget.create({ width: 200, height: 100 });
    await t.pumpView(withWindows(new ColoredBox({ color: Colors.white }), [{ widget: win, child: new SizedBox({}) }]));
    await t.pump();
    const routed: string[] = [];
    t.sendPointer(Phase.down, new Offset(20, 20));
    const capture = GestureBinding.instance.capturePointer((e) => routed.push(e.constructor.name))!;
    capture.moveToView(win.id);
    await t.flushMicrotasks();
    expect.isTrue(await mock.requestCloseView(win.id));
    expect.deepEqual(routed, ["PointerCancelEvent"]);
    expect.isFalse(capture.active);
  }, { mockOnly: true });

  testViews("Widget.create rejects when the host refuses to open a window", async (_t, ctx) => {
    const mock = ctx.host.mock!;
    mock.failNextCreateView = true;
    let error: unknown = null;
    try {
      await Widget.create({ width: 10, height: 10 });
    } catch (e) {
      error = e;
    }
    expect.isTrue(error !== null);
    expect.deepEqual(mock.openViews(), []);
  }, { mockOnly: true });
});
