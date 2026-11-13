// 在 mock 宿主下运行 examples/webview(多标签浏览器)并驱动交互: 新建/关闭标签(含缓动)、页面状态、地址栏。
import { openedUrls, viewCalls, webCommands } from "./browser_setup";
import { __mock } from "../mock/momo_ui";
import "../../../examples/webview/src/main";
import { IconButton, Spinner } from "../../../examples/webview/src/icons";
import { runRegisteredTests, test, expect } from "../harness";
import { Phase, WidgetTester, type PointerOptions } from "../tester";
import { LogicalKeyboardKey, Offset, PhysicalKeyboardKey, Size, defaultBinaryMessenger, TextField, utf8Encode } from "../../packages/momo/src/index";

/** 某个视图(窗口)最近一帧场景里的平台视图 id。 */
function platformViewsIn(view: number): number[] {
  return (__mock.sceneOpsByView.get(view) ?? []).filter((o) => o.op === "addPlatformView").map((o) => o.args[0] as number);
}

function created(): number[] {
  return viewCalls.filter((c) => c.method === "create").map((c) => c.id);
}

async function pushState(id: number, state: Record<string, unknown>): Promise<void> {
  const json = JSON.stringify({ id, event: "state", url: "", title: "", loading: false, progress: 1, canGoBack: false, canGoForward: false, ...state });
  await defaultBinaryMessenger.handlePlatformMessage("momo.webview/events", utf8Encode(json).slice().buffer);
}

// 地址栏聚焦时光标持续闪烁, pumpAndSettle 等不到静止: 按时长推进(覆盖 220ms 的标签动画)。
async function settle(t: WidgetTester): Promise<void> {
  await t.pumpFor(400);
}

// 拖动分步驱动(中途要断言, 不能用 t.drag 一次做完): 按下 / 按住移动 steps 步(每步一帧) / 抬起。
async function press(t: WidgetTester, at: Offset, o: PointerOptions = {}): Promise<void> {
  t.sendPointer(Phase.down, at, o);
  await t.flushMicrotasks();
}
async function moveHeld(t: WidgetTester, from: Offset, delta: Offset, steps: number, o: PointerOptions = {}): Promise<void> {
  for (let i = 1; i <= steps; i++) {
    t.now += 16;
    t.sendPointer(Phase.move, from.plus(delta.scale(i / steps)), o);
    await t.pump();
  }
}
async function release(t: WidgetTester, at: Offset): Promise<void> {
  t.sendPointer(Phase.up, at, { buttons: 0 });
  await settle(t);
}

test("examples/webview: open, switch and close tabs with eased insert/remove", async () => {
  const t = new WidgetTester(new Size(1000, 700));
  await settle(t);
  // 第一个浏览器窗口接管了启动窗口(视图 0): 标题跟随活动标签。
  expect.equal(__mock.titles.get(0), "新标签页");
  expect.equal(created().length, 1, "first tab creates one WebView");
  expect.equal(viewCalls[0]!.viewType, "momo.webview");
  const first = created()[0]!;

  // 页面状态 → 标签标题。
  await pushState(first, { title: "Example Domain", url: "https://example.com/" });
  await settle(t);
  expect.equal(t.byText("Example Domain").length, 1);
  // 加载中: 标签显示转圈图标, 结束后消失。
  await pushState(first, { title: "Example Domain", url: "https://example.com/", loading: true, progress: 0.3 });
  await t.pumpFor(100);
  expect.equal(t.byType(Spinner).length, 1, "spinner while loading");
  await pushState(first, { title: "Example Domain", url: "https://example.com/" });
  await settle(t);
  expect.equal(t.byType(Spinner).length, 0);

  // 新建标签: 插入动画进行中, 结束后两个标签都在。
  await t.tap(t.one(t.byText("+")));
  await t.pump(60);
  expect.equal(created().length, 2, "new tab creates a second WebView");
  expect.equal(t.byText("新标签页").length, 1, "new tab is inserted (animating)");
  await settle(t);
  const second = created()[1]!;
  expect.isTrue(second !== first);
  // 只有活动标签的平台视图进入场景(另一个 Offstage)。
  const views = __mock.lastSceneOps.filter((o) => o.op === "addPlatformView").map((o) => o.args[0]);
  expect.deepEqual(views, [second]);

  // 切回第一个标签: 地址栏显示其 URL, 场景里换成第一个视图, 第二个 WebView 不销毁。
  await t.tap(t.one(t.byText("Example Domain")));
  await settle(t);
  expect.deepEqual(__mock.lastSceneOps.filter((o) => o.op === "addPlatformView").map((o) => o.args[0]), [first]);
  expect.equal(t.widget(TextField).controller!.text, "https://example.com/");
  expect.isFalse(viewCalls.some((c) => c.method === "dispose"));

  // 标签右键 → 窗口内菜单。菜单与遮罩在平台视图之后加入场景, 即合成在网页之上。
  await t.tap(t.one(t.byText("Example Domain")), { buttons: 2 });
  await settle(t);
  expect.equal(t.byText("在新窗口中打开").length, 1);
  expect.equal(t.byText("在系统浏览器中打开").length, 1);
  const ops = __mock.lastSceneOps.map((o) => o.op);
  const pv = ops.indexOf("addPlatformView");
  expect.isTrue(pv >= 0 && ops.slice(pv + 1).includes("addPicture"), "menu is painted above the web view");
  await t.tap(t.one(t.byText("在系统浏览器中打开")));
  await settle(t);
  expect.deepEqual(openedUrls, ["https://example.com/"]);
  expect.equal(t.byText("在系统浏览器中打开").length, 0, "menu closed after selecting");

  // 点遮罩(网页区域)关闭菜单, 不触发任何动作。
  await t.tap(t.one(t.byText("Example Domain")), { buttons: 2 });
  await settle(t);
  await t.tap(new Offset(500, 500));
  await settle(t);
  expect.equal(t.byText("在新窗口中打开").length, 0, "tapping the dim barrier closes the menu");
  expect.equal(__mock.openViews().length, 0, "no action taken");

  // 关闭活动标签(×): 删除动画期间标签仍在, 结束后移除;其 WebView 立即销毁, 激活相邻标签。
  await t.tap(t.one(t.byText("×")));
  await t.pump(60);
  expect.equal(t.byText("Example Domain").length, 1, "closing tab still animating out");
  expect.deepEqual(viewCalls.filter((c) => c.method === "dispose").map((c) => c.id), [first]);
  await settle(t);
  expect.equal(t.byText("Example Domain").length, 0, "removed after the animation");
  expect.deepEqual(__mock.lastSceneOps.filter((o) => o.op === "addPlatformView").map((o) => o.args[0]), [second]);

  // 地址栏提交: 非网址按搜索。
  t.widget(TextField).controller!.text = "momo engine";
  await t.tap(t.one(t.byType(TextField)));
  await t.pressKey({ logical: LogicalKeyboardKey.enter.keyId, physical: PhysicalKeyboardKey.enter.keyId });
  await settle(t);
  const load = webCommands.filter((c) => c.method === "loadUrl").at(-1);
  expect.isTrue(load !== undefined && String(load.url).startsWith("https://duckduckgo.com/?q=momo%20engine"), JSON.stringify(load));

  // 起始页标签: 不能交给系统浏览器, 菜单里没有这一项。
  await t.tap(t.one(t.byText("新标签页")), { buttons: 2 });
  await settle(t);
  expect.equal(t.byText("在新窗口中打开").length, 1);
  expect.equal(t.byText("在系统浏览器中打开").length, 0, "hidden for the start page");
  await t.tap(new Offset(500, 500));
  await settle(t);

  // 新开一个标签, 再关掉旧的(关闭窗口的最后一个标签会关闭窗口, 见测试末尾)。
  await t.tap(t.one(t.byText("+")));
  await settle(t);
  expect.equal(created().length, 3);
  const oldTab = t.byText("新标签页").sort((a, b) => t.rectOf(a).left - t.rectOf(b).left)[0]!;
  await t.tap(oldTab);
  await settle(t);
  await t.tap(t.one(t.byText("×")));
  await settle(t);
  expect.equal(t.byText("新标签页").length, 1);

  // ---- 在新窗口中打开: 弹出到单独窗口(同一个 WebView, 不刷新)----
  const third = created().at(-1)!;
  await pushState(third, { title: "Docs", url: "https://docs.example/" });
  await settle(t);
  const createsBefore = created().length;
  await t.tap(t.one(t.byText("Docs")), { buttons: 2 });
  await settle(t);
  await t.tap(t.one(t.byText("在新窗口中打开")));
  await settle(t);
  expect.equal(__mock.openViews().length, 1);
  const win = __mock.openViews()[0]!;
  expect.equal(created().length, createsBefore, "the same web view moves; nothing is recreated");
  expect.deepEqual(platformViewsIn(win), [third]);
  expect.deepEqual(platformViewsIn(0), [], "the original tab shows a placeholder instead");
  expect.equal(t.byText("此标签页已在单独窗口中打开").length, 1);
  expect.equal(__mock.titles.get(win), "Docs");
  expect.isTrue(__mock.panelViews.has(win), "the separate window is a panel (key, not main, non-activating)");

  // 无边框: 两个窗口都隐藏系统标题栏, 只声明自己的标题栏高度(宿主让系统控制按钮在其中垂直居中)。
  // 标签栏顶部留 8pt 缩放边, 控制按钮与标签行(38pt)中线对齐: 声明高度 2 × (8 + 19) = 54;单独窗口顶栏 38pt。
  expect.deepEqual(__mock.titleBars.get(0), { hidden: true, height: 54 });
  expect.deepEqual(__mock.titleBars.get(win), { hidden: true, height: 38 });
  // 拖动区域由命中测试决定: 标签栏空白处(整条标签栏另有拖动识别器, 是它的祖先)与单独窗口的标题上按下都开始窗口拖动。
  await t.tap(new Offset(900, 25));
  await t.tap(new Offset(150, 19), { view: win });  // 单独窗口顶栏的标题文字上
  const drags = __mock.windowDrags.slice(-2);
  expect.deepEqual(drags.map((d) => d.view), [0, win]);
  expect.isTrue(drags.every((d) => d.sequenceId > 0) && drags[0]!.sequenceId !== drags[1]!.sequenceId, JSON.stringify(drags));

  // 置顶(图标按钮)、显示窗口。
  const [pin, restoreIcon] = t.byType(IconButton);
  expect.isTrue(pin !== undefined && restoreIcon !== undefined);
  await t.tap(pin!, { view: win });
  await settle(t);
  expect.equal(__mock.alwaysOnTop.get(win), true);
  expect.isTrue((t.byType(IconButton)[0]!.widget as IconButton).o.active === true, "pin shows as active");
  await t.tap(t.one(t.byText("显示窗口")));
  await settle(t);
  expect.deepEqual(__mock.activated, [win]);

  // 恢复到标签页(占位里的按钮): 窗口关闭, 网页移回原窗口, 不重建。
  await t.tap(t.one(t.byText("恢复到标签页")));
  await settle(t);
  expect.deepEqual(__mock.openViews(), []);
  expect.deepEqual(platformViewsIn(0), [third]);
  expect.equal(created().length, createsBefore);
  expect.isFalse(viewCalls.some((c) => c.method === "dispose" && c.id === third));

  // 再弹出一次, 然后用户关闭单独窗口: 原窗口的标签一起关闭, 网页销毁。先新开一个标签:
  // 否则那是原窗口的最后一个标签, 原窗口会随之关闭(所有窗口对等, 见测试末尾)。
  await t.tap(t.one(t.byText("+")));
  await settle(t);
  await t.tap(t.one(t.byText("Docs")), { buttons: 2 });
  await settle(t);
  await t.tap(t.one(t.byText("在新窗口中打开")));
  await settle(t);
  const win2 = __mock.openViews()[0]!;
  expect.deepEqual(platformViewsIn(win2), [third]);
  expect.isTrue(await __mock.requestCloseView(win2));
  await settle(t);
  expect.deepEqual(__mock.openViews(), []);
  expect.equal(t.byText("Docs").length, 0, "the tab is closed with its window");
  expect.isTrue(viewCalls.some((c) => c.method === "dispose" && c.id === third), "web view destroyed");

  // ---- 标签拖拽(类似 Chromium)----
  // 三个标签 One / Two / Three(宽 220, 第一个从 x = 78 开始)。
  await t.tap(t.one(t.byText("+")));
  await settle(t);
  await t.tap(t.one(t.byText("+")));
  await settle(t);
  const [one, two, three] = created().slice(-3);
  await pushState(one!, { title: "One", url: "https://one.example/" });
  await pushState(two!, { title: "Two", url: "https://two.example/" });
  await pushState(three!, { title: "Three", url: "https://three.example/" });
  await settle(t);
  const order = (): string[] =>
    ["One", "Two", "Three"].filter((n) => t.byText(n).length > 0).sort((a, b) => t.rectOf(t.byText(a)[0]!).left - t.rectOf(t.byText(b)[0]!).left);
  expect.deepEqual(order(), ["One", "Two", "Three"]);
  const webViewsBefore = created().length;

  // 排序: 按住 One 向右拖两格。拖动中其他标签平移让位;松手后顺序提交。
  const oneAt = t.centerOf(t.one(t.byText("One")));
  await press(t, oneAt);
  await moveHeld(t, oneAt, new Offset(440, 0), 8);
  await t.pumpFor(300);
  expect.equal(t.byText("One").length, 2, "placeholder + floating copy while dragging");
  await release(t, oneAt.plus(new Offset(440, 0)));
  expect.deepEqual(order(), ["Two", "Three", "One"]);
  expect.equal(t.byText("One").length, 1);

  // 拖出: 按住 Two 向下拖离标签栏 → 新窗口跟随指针, 同一个 WebView 移过去(不重建)。
  const twoAt = t.centerOf(t.one(t.byText("Two")));
  await press(t, twoAt);
  await moveHeld(t, twoAt, new Offset(120, 180), 6);
  await settle(t);
  expect.equal(__mock.openViews().length, 1, "torn-off tab opens a window");
  const torn = __mock.openViews()[0]!;
  expect.deepEqual(platformViewsIn(torn), [two!], "the web view moved into the new window");
  expect.equal(created().length, webViewsBefore, "nothing is recreated");
  expect.equal(t.byText("Two").length, 1, "only the new window's strip shows it");
  expect.isFalse(platformViewsIn(0).includes(two!), "the main window no longer renders it");
  // 序列的原生接收窗口改交新窗口(不合成按下), 宿主的移动循环让它跟随指针: 标签保持在指针下的按下位置
  // (控制按钮宽 78 + 按下点在标签内的偏移)。
  expect.equal(__mock.captures.at(-1), torn);
  expect.isTrue(__mock.moveLoops.has(torn), "the host moves the window");
  const far = twoAt.plus(new Offset(300, 260));
  await moveHeld(t, far, Offset.zero, 1);
  const grabX = twoAt.dx - 78;
  expect.equal(__mock.frames.get(torn)!.x, far.dx - 78 - grabX);
  expect.equal(__mock.frames.get(torn)!.y, far.dy - twoAt.dy);

  // 拖回主窗口的标签栏: 并入(插在指针处), 拖出的窗口关闭, 网页移回, 不重建。
  const backAt = new Offset(78 + 220 * 2 + 30, 19);
  await moveHeld(t, backAt, Offset.zero, 1);
  await settle(t);
  expect.deepEqual(__mock.openViews(), [], "the torn-off window closes after merging");
  await release(t, backAt);
  expect.deepEqual(order(), ["Three", "One", "Two"]);
  expect.deepEqual(platformViewsIn(0), [two!], "merged tab is active in the main window");
  expect.equal(created().length, webViewsBefore);
  expect.isFalse(viewCalls.some((c) => c.method === "dispose" && (c.id === one || c.id === two || c.id === three)));

  // ---- 单标签窗口整体并入另一个窗口: 触发拖动的窗口被关闭, 拖动会话(捕获者)照常接收, 原生接收窗口改交目标窗口 ----
  // 先把 Two 拖出并松手: 得到只有一个标签的窗口。
  const twoAgain = t.centerOf(t.one(t.byText("Two")));
  await press(t, twoAgain);
  await moveHeld(t, twoAgain, new Offset(0, 150), 5);
  await release(t, twoAgain.plus(new Offset(0, 150)));
  expect.equal(__mock.openViews().length, 1);
  const single = __mock.openViews()[0]!;
  expect.deepEqual(order().filter((n) => n !== "Two"), ["Three", "One"]);

  // 在单标签窗口里按住标签拖动 = 拖动整个窗口(宿主的移动循环);指针移到主窗口标签栏(屏幕坐标)即并入。
  __mock.captures.length = 0;
  const local = new Offset(78 + 110, 19);
  await press(t, local, { view: single });
  const f0 = __mock.frames.get(single)!;
  const startScreen = new Offset(f0.x + local.dx, f0.y + local.dy);
  const mergeAt = new Offset(78 + 220 + 30, 19);
  for (let i = 1; i <= 6; i++) {
    t.now += 16;
    const screen = startScreen.plus(mergeAt.minus(startScreen).scale(i / 6));
    const f = __mock.frames.get(single) ?? f0;
    t.sendPointer(Phase.move, new Offset(screen.dx - f.x, screen.dy - f.y), { view: single, screen });
    await t.flushMicrotasks();
    if (__mock.captures.length > 0) break;
    expect.isTrue(__mock.moveLoops.has(single), "the host moves the single-tab window");
    await t.pump();
  }
  expect.deepEqual(__mock.captures, [0], "the sequence's native receiver moves to the main window");
  // 同一个指针继续, 之后的事件由宿主改投主窗口(可能落在标签栏之外——合并判定有上下容差),
  // 它们直接交给拖动会话(捕获者), 不依赖命中测试。
  const handoffAt = new Offset(mergeAt.dx, -4);
  await moveHeld(t, handoffAt, Offset.zero, 1);
  await settle(t);
  expect.deepEqual(__mock.openViews(), [], "the emptied window closes");
  expect.deepEqual(order(), ["Three", "Two", "One"], "inserted under the pointer");
  expect.deepEqual(platformViewsIn(0), [two!]);

  // 主窗口的标签栏接着拖动排序。
  await moveHeld(t, handoffAt, new Offset(220, 0), 5);
  await release(t, handoffAt.plus(new Offset(220, 0)));
  expect.deepEqual(order(), ["Three", "One", "Two"], "the drag continues in the target window");
  expect.equal(created().length, webViewsBefore, "still the same web views");

  // 标签栏顶部 8pt 是窗口缩放边(交给系统): 从这里按下拖动不会拖起标签。
  const edge = new Offset(t.centerOf(t.one(t.byText("Three"))).dx, 3);
  await t.drag(edge, new Offset(0, 150), { steps: 6 });
  await settle(t);
  expect.deepEqual(__mock.openViews(), [], "no tab is torn off from the resize edge");
  expect.deepEqual(order(), ["Three", "One", "Two"]);

  // 所有窗口对等: 关闭启动窗口的最后一个标签即关闭这个窗口(宿主在最后一个窗口关闭后退出应用)。
  for (let i = 0; i < 3; i++) {
    await t.tap(t.one(t.byText("×")));
    await settle(t);
  }
  expect.isFalse(__mock.hasView(0), "the startup window closed with its last tab");
});

const proc = (globalThis as unknown as { process: { exitCode?: number } }).process;
const s = await runRegisteredTests({ name: "node-mock (browser example)", log: (l) => console.log(l), ahemText: true, mock: __mock });
if (s.failed > 0) proc.exitCode = 1;
