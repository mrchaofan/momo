// 多窗口: Widget(原生窗口对象)的方法与事件、RootView 把内容渲染进窗口、各窗口独立 MediaQuery/Overlay/渲染、
// 共享状态、GlobalKey 跨窗口迁移;app 的退出流程与窗口全部关闭。
import {
  AnimationController, Builder, Center, ColoredBox, Colors, Column, Focus, FocusManager, FocusNode, GestureDetector,
  GlobalKey, MediaQuery, MouseRegion, Offset, Positioned, RootView, SchedulerBinding, SizedBox, Stack, State,
  StatefulView, Text, Texture, TickerMode, ViewAnchor,
  Widget, WindowChrome, app, LogicalKeyboardKey, PhysicalKeyboardKey,
  ViewsBinding, EdgeInsets, GestureBinding, MediaQueryData, Rect, Size, runView, type BuildContext, type View,
} from "../packages/momo/src/index";
import { Kind, Phase } from "./tester";

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

/** 一直在跑的动画: 数它 tick 了多少次。 */
class Spinner extends StatefulView {
  constructor(readonly onTick: () => void) {
    super();
  }
  createState(): State {
    return new SpinnerState();
  }
}
class SpinnerState extends State<Spinner> {
  #controller!: AnimationController;
  override initState(): void {
    this.#controller = new AnimationController({ duration: 1000, vsync: this });
    this.#controller.addListener(() => this.view.onTick());
    void this.#controller.repeat();
  }
  override dispose(): void {
    this.#controller.dispose();
  }
  build(_c: BuildContext): View {
    return new SizedBox({ width: 10, height: 10 });
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
    expect.isFalse(ViewsBinding.instance.viewHosts.has(win.id), "the closed window's RootView is unregistered");
  }, { mockOnly: true });

  // 窗口关闭后仍留在树里的 RootView: 子树照常构建、状态保留, 但画面没有人看——其中的动画停下, 不再请求帧。
  testViews("a RootView left in the tree after its window closed stops its tickers", async (t) => {
    let ticks = 0;
    const win = await Widget.create({ width: 200, height: 100 });
    const root = withWindows(new SizedBox(), [{ widget: win, child: new Spinner(() => ticks++) }]);
    await t.pumpView(root);
    await t.pump(16);
    await t.pump(16);
    expect.isTrue(ticks > 0, "the animation runs while the window is open");
    expect.isTrue(SchedulerBinding.instance.hasScheduledFrame, "a running animation keeps requesting frames");

    win.destroy();
    await t.flushMicrotasks();
    await t.pump(16);  // RootView 重建: TickerMode 禁用, ticker 静音
    const afterClose = ticks;
    await t.pump(16);
    await t.pump(16);
    expect.equal(ticks, afterClose, "no ticks after the window closed");
    expect.isFalse(SchedulerBinding.instance.hasScheduledFrame, "and no frames are requested for it");
  }, { mockOnly: true });

  // TickerMode: 禁用时子树里 State 创建的 ticker 静音, 重新启用后继续;嵌套时任一层禁用即禁用。
  testViews("TickerMode mutes and resumes the tickers of its subtree", async (t) => {
    let ticks = 0;
    const tree = (outer: boolean, inner: boolean): View => new TickerMode({
      enabled: outer,
      child: new TickerMode({ enabled: inner, child: new Spinner(() => ticks++) }),
    });
    const settle = async (view: View): Promise<number> => {
      await t.pumpView(view);
      await t.pump(16);
      const before = ticks;
      await t.pump(16);
      await t.pump(16);
      return ticks - before;
    };
    expect.isTrue(await settle(tree(true, true)) > 0);
    expect.equal(await settle(tree(false, true)), 0, "disabled by an ancestor");
    expect.equal(await settle(tree(true, false)), 0, "disabled by the nearest TickerMode");
    expect.isTrue(await settle(tree(true, true)) > 0, "resumes when enabled again");
  });

  // 画面里有外部纹理的窗口每帧都提交: 纹理内容由扩展更新, 窗口自己没有变脏。别的窗口变脏引出的帧里, 没有纹理
  // 也没有变化的窗口不提交。
  testViews("a window showing an external texture is committed on every frame", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const video = await Widget.create({ width: 200, height: 100 });
    const idle = await Widget.create({ width: 200, height: 100 });
    await t.pumpView(withWindows(new SizedBox(), [
      { widget: video, child: new Texture({ textureId: 7 }) },
      { widget: idle, child: new SizedBox({ width: 10, height: 10 }) },
    ]));
    await t.pump();
    await t.pump();
    const videoScene = mock.sceneOpsByView.get(video.id);
    const idleScene = mock.sceneOpsByView.get(idle.id);
    expect.isTrue((videoScene ?? []).some((o) => o.op === "addTexture"), "the texture is in the window's scene");

    t.renderView.markNeedsPaint();  // 主窗口变脏: 框架请求的一帧
    expect.isTrue(SchedulerBinding.instance.hasScheduledFrame);
    await t.pump();
    expect.isTrue(mock.sceneOpsByView.get(video.id) !== videoScene, "the texture window got a new scene");
    expect.isTrue(mock.sceneOpsByView.get(idle.id) === idleScene, "the unchanged window was skipped");
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

  // 控制按钮在哪一侧由宿主报告的区域决定: 自绘标题栏用 windowControlsPadding 做两侧留白, 不判断平台。
  testViews("MediaQueryData.windowControlsPadding follows the side of the window controls", async () => {
    const size = new Size(800, 600);
    const left = new MediaQueryData({ size, windowControls: Rect.fromLTWH(0, 0, 78, 40) });  // macOS 红绿灯
    expect.equal(left.windowControlsPadding.left, 78);
    expect.equal(left.windowControlsPadding.right, 0);
    const right = new MediaQueryData({ size, windowControls: Rect.fromLTWH(662, 0, 138, 32) });  // Windows
    expect.equal(right.windowControlsPadding.left, 0);
    expect.equal(right.windowControlsPadding.right, 138);
    expect.equal(new MediaQueryData({ size }).windowControlsPadding, EdgeInsets.zero);
  });

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
    ViewsBinding.instance.viewHosts.get(win.id)!.renderView.markNeedsPaint();
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

  // runApp 就是视图 0 上的 RootView: 启动窗口与其他窗口登记在同一处。
  testViews("runApp renders the startup window through RootView", async (t) => {
    await t.pumpView(new Text("app"));
    const root = ViewsBinding.instance.rootView;
    expect.isTrue(root instanceof RootView && root.widget === Widget.fromId(0));
    expect.equal(ViewsBinding.instance.viewHosts.get(0)?.viewport, Widget.fromId(0)!.viewport);
    // 替换 app: 同一个 RootView(同一个登记), 不重建窗口的渲染树。
    const host = ViewsBinding.instance.viewHosts.get(0);
    await t.pumpView(new Text("app 2"));
    expect.equal(ViewsBinding.instance.viewHosts.get(0), host);
    expect.equal(t.byText("app 2").length, 1);
  });

  // 启动窗口在 runApp 之前就关闭了: app 照常构建, 不渲染到任何窗口。
  testViews("runApp without the startup window builds the app and renders nowhere", async (t, ctx) => {
    const mock = ctx.host.mock!;
    expect.isTrue(await mock.requestCloseView(0));
    await t.flushMicrotasks();
    const rendered = mock.renderedScenes;
    const sizes: string[] = [];
    await t.pumpView(new Builder({ builder: (c) => {
      sizes.push(MediaQuery.of(c).size.toString());
      return new Text("headless");
    } }));
    await t.pump();
    expect.deepEqual(sizes, [Size.zero.toString()]);
    expect.equal(t.byText("headless").length, 1);
    expect.equal(ViewsBinding.instance.viewHosts.size, 0);
    expect.equal(mock.renderedScenes, rendered);
    expect.isFalse(SchedulerBinding.instance.hasScheduledFrame, "a tree without a window requests no frames");
  }, { mockOnly: true });

  // 没有脏节点的窗口不重复合成、提交;首帧、尺寸变化、重新可见、引擎自己发起的帧照常提交。
  testViews("only windows whose render tree changed are re-submitted", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const key = new GlobalKey<CounterState>();
    const win = await Widget.create({ width: 200, height: 100 });
    await t.pumpView(withWindows(new Counter({ key }), [{ widget: win, child: new ColoredBox({ color: Colors.white }) }]));
    await t.pump();
    expect.isTrue(mock.sceneOpsByView.has(win.id), "first frame");

    // 只有主窗口的内容变了。
    mock.sceneOpsByView.delete(win.id);
    let rendered = mock.renderedScenes;
    key.currentState!.count = 1;
    key.currentState!.setState();
    await t.pump();
    expect.equal(mock.renderedScenes, rendered + 1, "the changed window is submitted");
    expect.isFalse(mock.sceneOpsByView.has(win.id), "the unchanged window is not");

    // 框架请求了帧, 但渲染树没有变化: 都不提交。
    rendered = mock.renderedScenes;
    SchedulerBinding.instance.scheduleFrame();
    await t.pump();
    expect.equal(mock.renderedScenes, rendered);
    expect.isFalse(mock.sceneOpsByView.has(win.id));

    // 尺寸变化: 提交。
    t.setViewSize(new Size(640, 480));
    await t.pump();
    expect.equal(mock.renderedScenes, rendered + 1, "resize");

    // 隐藏期间不提交(哪怕有变化), 重新可见后补一帧。
    win.hide();
    await t.flushMicrotasks();
    ViewsBinding.instance.viewHosts.get(win.id)!.renderView.markNeedsPaint();
    await t.pump();
    expect.isFalse(mock.sceneOpsByView.has(win.id));
    win.show();
    await t.flushMicrotasks();
    expect.isTrue(SchedulerBinding.instance.hasScheduledFrame, "becoming visible requests a frame");
    rendered = mock.renderedScenes;
    await t.pump();
    expect.isTrue(mock.sceneOpsByView.has(win.id), "visible again");
    expect.equal(mock.renderedScenes, rendered, "the other window is left alone");

    // 引擎自己发起的帧(框架没有请求): 每个可见窗口都提交。
    mock.sceneOpsByView.delete(win.id);
    expect.isFalse(SchedulerBinding.instance.hasScheduledFrame);
    await t.pump();
    expect.isTrue(mock.sceneOpsByView.has(win.id));
    expect.equal(mock.renderedScenes, rendered + 1);
  }, { mockOnly: true });

  // 默认快捷键(Tab 遍历)在每个窗口里都生效: 窗口的作用域在默认快捷键的节点之下。
  testViews("default shortcuts work in every window", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const a = new FocusNode({ debugLabel: "a" });
    const b = new FocusNode({ debugLabel: "b" });
    const win = await Widget.create({ width: 200, height: 100 });
    const box = (node: FocusNode): View => new Focus({ focusNode: node, child: new SizedBox({ width: 20, height: 20 }) });
    await t.pumpView(withWindows(new SizedBox({}), [{ widget: win, child: new Column({ children: [box(a), box(b)] }) }]));
    await t.pump();
    mock.setWindowFocus(win.id, true);
    await t.pump();
    const tab = { logical: LogicalKeyboardKey.tab.keyId, physical: PhysicalKeyboardKey.tab.keyId };
    await t.pressKey(tab);
    expect.equal(FocusManager.instance.primaryFocus, a);
    await t.pressKey(tab);
    expect.equal(FocusManager.instance.primaryFocus, b);
  }, { mockOnly: true });

  // 布局变化后悬停状态按新布局重新计算(指针没动), 不论是哪个窗口。
  testViews("a layout change in any window re-evaluates hover", async (t) => {
    const win = await Widget.create({ width: 200, height: 100 });
    const log: string[] = [];
    const content = (left: number): View => new Stack({ children: [
      new Positioned({ left, top: 0, width: 40, height: 40, child: new MouseRegion({
        onEnter: () => log.push("enter"), onExit: () => log.push("exit"), child: new ColoredBox({ color: Colors.white }),
      }) }),
    ] });
    const root = (left: number): View => withWindows(new SizedBox({}), [{ widget: win, child: content(left) }]);
    await t.pumpView(root(120));
    await t.pump();
    t.sendPointer(Phase.hover, new Offset(20, 20), { kind: Kind.mouse, buttons: 0, view: win.id });
    await t.pump();
    expect.deepEqual(log, []);
    await t.pumpView(root(0));  // 区域移到指针下面
    await t.pump();
    expect.deepEqual(log, ["enter"]);
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
