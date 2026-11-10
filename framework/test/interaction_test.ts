import {
  Center, Column, Container, Colors, ElevatedButton, Expanded, FocusManager, FocusNode, Focus, ListView, LogicalKeyboardKey,
  PhysicalKeyboardKey, RenderListViewport, ScrollController, SingleChildScrollView, SizedBox, Text, ValueKey, Offset, Row,
  Shortcuts, Actions, SingleActivator, Intent, CallbackAction, KeyModifier, AnimatedOpacity, Opacity, RenderOpacity,
  type Widget, Theme, ThemeData, GestureDetector,
} from "../packages/momo/src/index";
import { expect, group } from "./harness";
import { Kind } from "./tester";
import { testWidgets } from "./util";

const item = (i: number): Widget => new Container({ key: new ValueKey(i), height: 50, color: Colors.grey200, child: new Text(`item ${i}`) });

const keys = {
  tab: { logical: LogicalKeyboardKey.tab.keyId, physical: PhysicalKeyboardKey.tab.keyId },
  enter: { logical: LogicalKeyboardKey.enter.keyId, physical: PhysicalKeyboardKey.enter.keyId },
  space: { logical: LogicalKeyboardKey.space.keyId, physical: PhysicalKeyboardKey.space.keyId, character: " " },
  keyK: { logical: LogicalKeyboardKey.keyK.keyId, physical: PhysicalKeyboardKey.keyK.keyId, character: "k" },
};

group("scrolling", () => {
  testWidgets("ListView.builder builds lazily and scrolls with the wheel", async (t) => {
    const controller = new ScrollController();
    let built = 0;
    await t.pumpWidget(ListView.builder({
      controller,
      itemCount: 1000,
      cacheExtent: 0,
      itemBuilder: (_c, i) => {
        built++;
        return item(i);
      },
    }));
    const vp = t.allElements().map((e) => e.findRenderObject()).find((r): r is RenderListViewport => r instanceof RenderListViewport)!;
    // 600 高视口 / 50 高的项 → 0..12(含边界项)
    expect.deepEqual(vp.builtIndices, Array.from({ length: 13 }, (_, i) => i));
    expect.isTrue(built <= 13, `built ${built}`);
    expect.equal(t.byText("item 0").length, 1);
    await t.scroll(new Offset(400, 300), 120);
    expect.equal(controller.offset, 120);
    expect.deepEqual(vp.builtIndices[0], 2);
    expect.equal(t.byText("item 0").length, 0, "scrolled-off item is disposed");
    expect.equal(t.rectOf(t.one(t.byText("item 3"))).top, 30);
    // 滚到底被钳制
    controller.jumpTo(1e9);
    await t.pump();
    expect.equal(controller.offset, 1000 * 50 - 600);
    expect.isTrue(vp.builtIndices.includes(999));
    await t.scroll(new Offset(400, 300), 100);
    expect.equal(controller.offset, 1000 * 50 - 600);
  });

  testWidgets("touch drag scrolls and flings; mouse drag does not", async (t) => {
    const controller = new ScrollController();
    await t.pumpWidget(new ListView({ controller, children: Array.from({ length: 100 }, (_, i) => item(i)) }));
    await t.drag(new Offset(400, 400), new Offset(0, -200), { kind: Kind.touch, steps: 10, stepMs: 10 });
    const afterDrag = controller.offset;
    expect.isTrue(afterDrag > 150 && afterDrag <= 200, `after drag ${afterDrag}`);
    await t.pumpAndSettle();
    expect.isTrue(controller.offset > afterDrag + 50, `fling continued: ${controller.offset}`);
    const before = controller.offset;
    await t.drag(new Offset(400, 400), new Offset(0, -200), { kind: Kind.mouse });
    expect.equal(controller.offset, before);
  });

  testWidgets("animateTo and SingleChildScrollView clipping", async (t) => {
    const controller = new ScrollController();
    await t.pumpWidget(new SingleChildScrollView({
      controller,
      child: new Column({ children: Array.from({ length: 30 }, (_, i) => item(i)) }),
    }));
    expect.equal(controller.position.maxScrollExtent, 30 * 50 - 600);
    void controller.animateTo(500, { duration: 200 });
    await t.pumpAndSettle();
    expect.equal(controller.offset, 500);
    expect.equal(t.rectOf(t.one(t.byText("item 10"))).top, 0);
  });

  testWidgets("nested scrollables: innermost consumes the wheel", async (t) => {
    const outer = new ScrollController();
    const inner = new ScrollController();
    await t.pumpWidget(new ListView({
      controller: outer,
      children: [
        new SizedBox({ height: 200, child: new ListView({ controller: inner, children: Array.from({ length: 20 }, (_, i) => item(i)) }) }),
        ...Array.from({ length: 20 }, (_, i) => item(100 + i)),
      ],
    }));
    await t.scroll(new Offset(100, 100), 50);
    expect.deepEqual([inner.offset, outer.offset], [50, 0]);
    await t.scroll(new Offset(100, 400), 50);
    expect.deepEqual([inner.offset, outer.offset], [50, 50]);
  });

  testWidgets("ListView updates items when data changes", async (t) => {
    await t.pumpWidget(new ListView({ children: [item(1), item(2), item(3)] }));
    expect.equal(t.byText("item 2").length, 1);
    await t.pumpWidget(new ListView({ children: [item(1), item(3)] }));
    expect.equal(t.byText("item 2").length, 0);
    expect.equal(t.rectOf(t.one(t.byText("item 3"))).top, 50);
  });
});

class IncIntent extends Intent {}

group("focus & shortcuts", () => {
  testWidgets("Tab / Shift+Tab traverse in reading order; Enter activates", async (t) => {
    const pressed: string[] = [];
    const nodes = ["a", "b", "c"].map((n) => new FocusNode({ debugLabel: n }));
    await t.pumpWidget(new Theme({
      data: new ThemeData(),
      child: new Column({
        children: [
          new Row({ children: [
            new ElevatedButton({ focusNode: nodes[1]!, onPressed: () => pressed.push("b"), child: new Text("B") }),
            new ElevatedButton({ focusNode: nodes[0]!, onPressed: () => pressed.push("a"), child: new Text("A") }),
          ] }),
          new ElevatedButton({ focusNode: nodes[2]!, onPressed: () => pressed.push("c"), child: new Text("C") }),
        ],
      }),
    }));
    // 阅读顺序:B(左上)→ A(右上)→ C(下)
    await t.pressKey(keys.tab);
    expect.equal(FocusManager.instance.primaryFocus, nodes[1]);
    await t.pressKey(keys.tab);
    expect.equal(FocusManager.instance.primaryFocus, nodes[0]);
    await t.pressKey(keys.tab);
    expect.equal(FocusManager.instance.primaryFocus, nodes[2]);
    await t.pressKey(keys.tab);
    expect.equal(FocusManager.instance.primaryFocus, nodes[1], "wraps around");
    t.sendKeyData("down", LogicalKeyboardKey.shiftLeft.keyId, PhysicalKeyboardKey.shiftLeft.keyId, { modifiers: KeyModifier.shift });
    await t.pressKey(keys.tab, KeyModifier.shift);
    t.sendKeyData("up", LogicalKeyboardKey.shiftLeft.keyId, PhysicalKeyboardKey.shiftLeft.keyId);
    expect.equal(FocusManager.instance.primaryFocus, nodes[2]);
    const handled = await t.pressKey(keys.enter);
    expect.isTrue(handled);
    await t.pressKey(keys.space);
    expect.deepEqual(pressed, ["c", "c"]);
    expect.isTrue(nodes[2]!.hasFocus);
  });

  testWidgets("key events bubble from the primary focus; Shortcuts map to Actions", async (t) => {
    let count = 0;
    const seen: string[] = [];
    const inner = new FocusNode({ debugLabel: "inner" });
    await t.pumpWidget(new Shortcuts({
      shortcuts: new Map([[new SingleActivator(LogicalKeyboardKey.keyK, { control: true }), new IncIntent()]]),
      child: new Actions({
        actions: new Map([[IncIntent, new CallbackAction<IncIntent>({ onInvoke: () => count++ })]]),
        child: new Focus({
          onKeyEvent: (_n, e) => {
            seen.push(`outer:${e.logicalKey.debugName}`);
            return "ignored";
          },
          child: new Focus({ focusNode: inner, autofocus: true, child: new SizedBox({ width: 10, height: 10 }) }),
        }),
      }),
    }));
    expect.equal(FocusManager.instance.primaryFocus, inner, "autofocus");
    expect.isFalse(await t.pressKey(keys.keyK));
    expect.equal(count, 0);
    expect.isTrue(await t.pressKey(keys.keyK, KeyModifier.control));
    expect.equal(count, 1);
    expect.isTrue(seen.length >= 2 && seen[0]!.startsWith("outer:"));
  });

  testWidgets("unfocus returns focus to the scope; removed node loses focus", async (t) => {
    const node = new FocusNode({ debugLabel: "n" });
    let show = true;
    const build = (): Widget => new Center({ child: show ? new Focus({ focusNode: node, child: new SizedBox({ width: 5, height: 5 }) }) : new SizedBox() });
    await t.pumpWidget(build());
    node.requestFocus();
    expect.isTrue(node.hasPrimaryFocus);
    node.unfocus();
    expect.isFalse(node.hasFocus);
    node.requestFocus();
    show = false;
    await t.pumpWidget(build());
    expect.isFalse(node.hasFocus);
    expect.notNull(FocusManager.instance.primaryFocus);
  });
});

group("implicit animation", () => {
  testWidgets("AnimatedOpacity animates and composites with an OpacityLayer", async (t) => {
    let target = 1;
    const build = (): Widget => new Center({ child: new AnimatedOpacity({ opacity: target, duration: 200, child: new Container({ width: 10, height: 10, color: Colors.red }) }) });
    await t.pumpWidget(build());
    const ro = (): RenderOpacity => t.one(t.byType(Opacity)).findRenderObject() as RenderOpacity;
    expect.equal(ro().opacity, 1);
    target = 0;
    await t.pumpWidget(build());
    await t.pump(100);
    const mid = ro().opacity;
    expect.isTrue(mid > 0.3 && mid < 0.7, `mid ${mid}`);
    expect.isTrue(ro().alwaysNeedsCompositing);
    await t.pumpAndSettle();
    expect.equal(ro().opacity, 0);
    void GestureDetector;
    void Expanded;
  });
});
