// 多视图(多窗口): Window 声明式开关窗口、各窗口独立 MediaQuery/Overlay/渲染、共享状态、GlobalKey 跨窗口迁移。
import {
  Builder, Center, ColoredBox, Colors, Focus, FocusManager, FocusNode, GlobalKey, MediaQuery, SizedBox, State,
  StatefulWidget, Text, ViewAnchor, Window, WindowDragArea, WidgetsBinding, type BuildContext, type Widget,
} from "../packages/momo/src/index";
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

  testWidgets("closed windows are not retained by drag-region bookkeeping", async (t, ctx) => {
    const mock = ctx.host.mock!;
    const app = (open: boolean): Widget => new ViewAnchor({
      child: new SizedBox({}),
      views: open ? [new Window({
        width: 200, height: 100,
        child: new WindowDragArea({ child: new SizedBox({ width: 50, height: 20 }) }),
      })] : [],
    });
    for (let round = 0; round < 3; round++) {
      await t.pumpWidget(app(true));
      await t.pump();
      await t.pump();
      expect.equal(mock.openViews().length, 1);
      expect.equal(WidgetsBinding.instance.debugDragRegionViewCount, 1);
      // 宿主在帧外关闭窗口(如点关闭按钮), 之后不再推进帧: 视图移除的通知本身就要清掉记录。
      expect.isTrue(mock.requestCloseView(mock.openViews()[0]!));
      for (let i = 0; i < 3; i++) await Promise.resolve();
      expect.deepEqual(mock.openViews(), []);
      expect.equal(WidgetsBinding.instance.debugDragRegionViewCount, 0, `round ${round}`);
      await t.pumpWidget(app(false));
    }
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
    expect.isFalse(mock.requestCloseView(id), "onCloseRequested returned false");
    expect.deepEqual(mock.openViews(), [id]);
    allow = true;
    expect.isTrue(mock.requestCloseView(id));
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
