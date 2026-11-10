// 在 mock 宿主下运行 examples/webview(多标签浏览器)并驱动交互: 新建/关闭标签(含缓动)、页面状态、地址栏。
import { openedUrls, viewCalls, webCommands } from "./browser_setup";
import { __mock } from "../mock/momo_ui";
import "../../../examples/webview/src/main";
import { IconButton } from "../../../examples/webview/src/icons";
import { runRegisteredTests, test, expect } from "../harness";
import { WidgetTester } from "../tester";
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

test("examples/webview: open, switch and close tabs with eased insert/remove", async () => {
  const t = new WidgetTester(new Size(1000, 700));
  await settle(t);
  expect.equal(__mock.title, "Momo Browser");
  expect.equal(created().length, 1, "first tab creates one WebView");
  expect.equal(viewCalls[0]!.viewType, "momo.webview");
  const first = created()[0]!;

  // 页面状态 → 标签标题。
  await pushState(first, { title: "Example Domain", url: "https://example.com/" });
  await settle(t);
  expect.equal(t.byText("Example Domain").length, 1);

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

  // 关闭最后一个标签: 自动新开一个。
  await t.tap(t.one(t.byText("×")));
  await settle(t);
  expect.equal(created().length, 3);
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

  // 无边框: 两个窗口都隐藏系统标题栏, 红绿灯与 38pt 顶栏居中;顶栏空白处登记为拖动区域。
  expect.deepEqual(__mock.titleBars.get(0), { style: "hidden", center: 19 });
  expect.deepEqual(__mock.titleBars.get(win), { style: "hidden", center: 19 });
  expect.isTrue((__mock.dragRegions.get(0) ?? []).length >= 8, "main window: traffic-light spacer + tab strip blank");
  expect.isTrue((__mock.dragRegions.get(win) ?? []).length >= 8, "separate window: title bar");

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

  // 再弹出一次, 然后用户关闭单独窗口: 原窗口的标签一起关闭, 网页销毁。
  await t.tap(t.one(t.byText("Docs")), { buttons: 2 });
  await settle(t);
  await t.tap(t.one(t.byText("在新窗口中打开")));
  await settle(t);
  const win2 = __mock.openViews()[0]!;
  expect.deepEqual(platformViewsIn(win2), [third]);
  expect.isTrue(__mock.requestCloseView(win2));
  await settle(t);
  expect.deepEqual(__mock.openViews(), []);
  expect.equal(t.byText("Docs").length, 0, "the tab is closed with its window");
  expect.isTrue(viewCalls.some((c) => c.method === "dispose" && c.id === third), "web view destroyed");
});

const proc = (globalThis as unknown as { process: { exitCode?: number } }).process;
const s = await runRegisteredTests({ name: "node-mock (browser example)", log: (l) => console.log(l), ahemText: true, mock: __mock });
if (s.failed > 0) proc.exitCode = 1;
