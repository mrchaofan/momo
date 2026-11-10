import {
  Align, Alignment, BoxConstraints, Center, Column, ConstrainedBox, Container, EdgeInsets, Expanded, Flexible, Padding,
  Positioned, RenderBox, RenderFlex, RenderParagraph, Row, SizedBox, Stack, Text, TextStyle, ValueKey, type Element,
  RenderPadding, RenderConstrainedBox, RenderPositionedBox, Size, Offset, RenderStack, RenderView, PipelineOwner,
  BoxParentData, IgnorePointer, Transform, Matrix4, HitTestResult, GestureBinding, Listener,
} from "../packages/momo/src/index";
import { expect, group, test } from "./harness";
import { testWidgets } from "./util";

const box = (k: string, w: number, h: number): SizedBox => new SizedBox({ key: new ValueKey(k), width: w, height: h });

group("rendering/layout", () => {
  testWidgets("Row: flex distribution and spaceBetween", async (t) => {
    await t.pumpWidget(new Column({
      children: [
        new Row({ children: [box("a", 100, 10), new Expanded({ flex: 1, child: box("b", 0, 10) }), new Expanded({ flex: 3, child: box("c", 0, 10) })] }),
        new Row({ mainAxisAlignment: "spaceBetween", children: [box("d", 100, 20), box("e", 100, 20), box("f", 100, 20)] }),
        new Row({ mainAxisAlignment: "center", children: [box("g", 100, 30), box("h", 100, 30)] }),
      ],
    }));
    const r = (k: string): ReturnType<typeof t.rectOf> => t.rectOf(t.one(t.byKey(new ValueKey(k))));
    expect.equal(r("b").width, 175);
    expect.equal(r("c").width, 525);
    expect.equal(r("c").left, 275);
    expect.deepEqual([r("d").left, r("e").left, r("f").left], [0, 350, 700]);
    expect.deepEqual([r("g").left, r("h").left], [300, 400]);
    // Column 默认交叉轴居中, 每行高度 = 子最大高度
    expect.deepEqual([r("a").top, r("d").top, r("g").top], [0, 10, 30]);
  });

  testWidgets("Column: mainAxisSize.min, crossAxis stretch/end, Flexible loose", async (t) => {
    await t.pumpWidget(new Center({
      child: new Column({
        mainAxisSize: "min",
        crossAxisAlignment: "end",
        children: [box("a", 50, 10), box("b", 120, 20)],
      }),
    }));
    const col = t.one(t.byType(Column)).findRenderObject() as RenderFlex;
    expect.deepEqual([col.size.width, col.size.height], [120, 30]);
    expect.equal(t.rectOf(t.one(t.byKey(new ValueKey("a")))).left, 340 + 70);
    await t.pumpWidget(new Row({
      crossAxisAlignment: "stretch",
      children: [new Flexible({ child: box("x", 100, 10) }), new Expanded({ child: new SizedBox({ key: new ValueKey("y") }) })],
    }));
    const x = t.rectOf(t.one(t.byKey(new ValueKey("x"))));
    const y = t.rectOf(t.one(t.byKey(new ValueKey("y"))));
    expect.deepEqual([x.width, x.height], [100, 600], "loose flexible keeps own width, stretch tightens height");
    expect.deepEqual([y.left, y.width], [100, 400]);
  });

  testWidgets("Stack: positioned + alignment", async (t) => {
    await t.pumpWidget(new Center({
      child: new SizedBox({
        width: 200, height: 100,
        child: new Stack({
          alignment: Alignment.bottomRight,
          children: [
            SizedBox.expand(),
            box("np", 20, 10),
            new Positioned({ left: 10, top: 5, child: box("lt", 30, 30) }),
            new Positioned({ right: 10, bottom: 5, width: 40, child: box("rb", 0, 20) }),
            new Positioned({ left: 10, right: 10, top: 40, child: box("stretch", 0, 10) }),
            Positioned.fill({ left: 1, top: 2, right: 3, bottom: 4, child: new SizedBox({ key: new ValueKey("fill") }) }),
          ],
        }),
      }),
    }));
    const r = (k: string): ReturnType<typeof t.rectOf> => t.rectOf(t.one(t.byKey(new ValueKey(k))));
    // Stack 位于 (300, 250)
    expect.deepEqual([r("np").left, r("np").top], [300 + 180, 250 + 90]);
    expect.deepEqual([r("lt").left, r("lt").top], [310, 255]);
    expect.deepEqual([r("rb").left, r("rb").top, r("rb").width, r("rb").height], [300 + 150, 250 + 75, 40, 20]);
    expect.deepEqual([r("stretch").left, r("stretch").width], [310, 180]);
    expect.deepEqual([r("fill").left, r("fill").top, r("fill").width, r("fill").height], [301, 252, 196, 94]);
  });

  testWidgets("Padding / Align / Container constraints", async (t) => {
    await t.pumpWidget(new Align({
      alignment: Alignment.topLeft,
      child: new Container({
        margin: EdgeInsets.all(10),
        padding: EdgeInsets.fromLTRB(1, 2, 3, 4),
        width: 100,
        height: 50,
        child: new Align({ alignment: Alignment.bottomRight, child: box("in", 10, 10) }),
      }),
    }));
    const r = t.rectOf(t.one(t.byKey(new ValueKey("in"))));
    // 内容区:x 11..107, y 12..56 → bottomRight 子在 (97, 46)
    expect.deepEqual([r.left, r.top], [97, 46]);
    await t.pumpWidget(new Center({ child: new ConstrainedBox({ constraints: BoxConstraints.from({ minWidth: 50, maxHeight: 20 }), child: box("c", 10, 100) }) }));
    const c = t.rectOf(t.one(t.byKey(new ValueKey("c"))));
    expect.deepEqual([c.width, c.height], [50, 20]);
  });

  testWidgets("Text uses Ahem metrics; wraps at constraint", async (t) => {
    await t.pumpWidget(new Center({ child: new SizedBox({ width: 100, child: new Text("hello world", { style: new TextStyle({ fontSize: 10 }) }) }) }));
    const p = t.one(t.byText("hello world")).findRenderObject() as RenderParagraph;
    // "hello world" = 110px > 100 → 两行 "hello " / "world"
    expect.deepEqual([p.size.width, p.size.height], [100, 20]);
    expect.equal(p.textPainter.computeLineMetrics().length, 2);
    await t.pumpWidget(new Center({ child: new Text("abc", { style: new TextStyle({ fontSize: 20 }) }) }));
    const q = t.one(t.byText("abc")).findRenderObject() as RenderParagraph;
    expect.deepEqual([q.size.width, q.size.height], [60, 20]);
    expect.close(q.getDistanceToBaseline("alphabetic")!, 16);
  }, { ahemText: true });

  testWidgets("Row baseline alignment", async (t) => {
    await t.pumpWidget(new Align({
      alignment: Alignment.topLeft,
      child: new Row({
        crossAxisAlignment: "baseline",
        textBaseline: "alphabetic",
        children: [new Text("A", { style: new TextStyle({ fontSize: 10 }) }), new Text("B", { style: new TextStyle({ fontSize: 30 }) })],
      }),
    }));
    const a = t.rectOf(t.one(t.byText("A")));
    const b = t.rectOf(t.one(t.byText("B")));
    // 基线:A 8, B 24 → A 下移 16
    expect.deepEqual([a.top, b.top], [16, 0]);
  }, { ahemText: true });

  testWidgets("relayout boundary: child size change re-lays out parent", async (t) => {
    await t.pumpWidget(new Align({ alignment: Alignment.topLeft, child: new Padding({ padding: EdgeInsets.all(5), child: box("a", 10, 10) }) }));
    const pad = t.one(t.byType(Padding)).findRenderObject() as RenderPadding;
    expect.deepEqual([pad.size.width, pad.size.height], [20, 20]);
    const inner = t.one(t.byKey(new ValueKey("a"))).findRenderObject() as RenderConstrainedBox;
    inner.additionalConstraints = BoxConstraints.tight(new Size(30, 40));
    await t.pump();
    expect.deepEqual([pad.size.width, pad.size.height], [40, 50]);
  });
});

group("rendering/pipeline (no widgets)", () => {
  test("PipelineOwner flushLayout on a raw render tree", () => {
    const owner = new PipelineOwner();
    const leaf = new RenderConstrainedBox(BoxConstraints.tight(new Size(10, 20)));
    const pos = new RenderPositionedBox({ alignment: Alignment.center }, leaf);
    const view = new RenderView({ size: new Size(100, 100), devicePixelRatio: 2 }, pos);
    owner.rootNode = view;
    view.prepareInitialFrame();
    owner.flushLayout();
    expect.deepEqual((leaf.parentData as BoxParentData).offset.dx, 45);
    expect.equal(leaf.localToGlobal(Offset.zero).dy, 40);
    expect.isFalse(leaf.needsLayout);
    leaf.additionalConstraints = BoxConstraints.tight(new Size(50, 50));
    expect.isTrue(pos.needsLayout || leaf.needsLayout);
    owner.flushLayout();
    expect.equal((leaf.parentData as BoxParentData).offset.dx, 25);
    void RenderBox;
    void RenderStack;
  });
});

group("rendering/hit test", () => {
  testWidgets("hit test path: topmost stack child first; IgnorePointer; Transform", async (t) => {
    const hits: string[] = [];
    const listener = (name: string, child: SizedBox): Listener => new Listener({ onPointerDown: () => hits.push(name), behavior: "opaque", child });
    await t.pumpWidget(new Stack({
      children: [
        Positioned.fill({ child: listener("bottom", new SizedBox()) }),
        new Positioned({ left: 0, top: 0, width: 100, height: 100, child: listener("top", new SizedBox()) }),
        new Positioned({ left: 200, top: 0, width: 100, height: 100, child: new IgnorePointer({ child: listener("ignored", new SizedBox()) }) }),
        new Positioned({ left: 400, top: 0, width: 100, height: 100, child: new Transform({ transform: Matrix4.translationValues(0, 200), child: listener("moved", new SizedBox()) }) }),
      ],
    }));
    const tap = async (x: number, y: number): Promise<void> => {
      hits.length = 0;
      await t.tap(new Offset(x, y));
    };
    await tap(50, 50);
    expect.deepEqual(hits, ["top"]);
    await tap(250, 50);
    expect.deepEqual(hits, ["bottom"]);
    await tap(450, 250);
    expect.deepEqual(hits, ["moved"]);
    await tap(450, 50);
    expect.deepEqual(hits, ["bottom"]);
    // 局部坐标经变换
    const result = new HitTestResult();
    GestureBinding.instance.hitTestInView!(result, new Offset(410, 220), 0);
    const entry = result.path.find((e) => e.target instanceof Object && e.target.constructor.name === "RenderPointerListener");
    expect.notNull(entry);
    expect.deepEqual(entry!.transform!.transformPoint(new Offset(410, 220)).toString(), new Offset(10, 20).toString());
  });

  testWidgets("devicePixelRatio: physical pointer coords map to logical", async (t) => {
    const at: Offset[] = [];
    await t.pumpWidget(new Align({
      alignment: Alignment.topLeft,
      child: new Padding({ padding: EdgeInsets.all(10), child: new Listener({ behavior: "opaque", onPointerDown: (e) => at.push(e.localPosition), child: box("l", 50, 50) }) }),
    }));
    await t.tap(new Offset(20, 30));
    expect.equal(at.length, 1);
    expect.deepEqual([at[0]!.dx, at[0]!.dy], [10, 20]);
  }, { dpr: 2 });
});

void (null as unknown as Element);
