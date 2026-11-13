import {
  Align, Alignment, runApp, setErrorReporter, GestureDetector, MouseRegion, Offset, PointerEventConverter, PointerDownEvent, PointerMoveEvent,
  PointerUpEvent, PointerAddedEvent, PointerScrollEvent, Stack, Positioned, ValueKey, Center, Column,
  GestureArenaManager, Container, Colors, GestureBinding, type PointerCapture, type View, type GestureArenaMember, type DragUpdateDetails, type DragEndDetails,
} from "../packages/momo/src/index";
import { expect, group, test } from "./harness";
import { Kind, Phase } from "./tester";
import { testViews } from "./util";

const square = (k: string, size = 100): View => new Container({ key: new ValueKey(k), width: size, height: size, color: Colors.blue });

group("gestures", () => {
  testViews("pointer events before the first frame are ignored, not thrown", async (t) => {
    const errors: string[] = [];
    const old = setErrorReporter((e, ctx) => errors.push(`${ctx}: ${e instanceof Error ? e.message : String(e)}`));
    try {
      let taps = 0;
      // runApp 同步建树但不布局;首帧前的 hover/add/down 不应在未布局的子树上做命中测试。
      runApp(new Stack({ children: [new GestureDetector({ onTap: () => taps++, child: square("a") })] }));
      t.sendPointer(Phase.hover, new Offset(10, 10));
      t.sendPointer(Phase.down, new Offset(10, 10));
      t.sendPointer(Phase.up, new Offset(10, 10), { buttons: 0 });
      expect.deepEqual(errors, []);
      expect.equal(taps, 0, "nothing is hit before layout");
      await t.pump();
      await t.tap(new Offset(10, 10));
      expect.equal(taps, 1, "hit testing works after the first frame");
      expect.deepEqual(errors, []);
    } finally {
      setErrorReporter(old);
    }
  });

  test("PointerEventConverter: dpr scaling, synthesized add, unique pointer ids", () => {
    const c = new PointerEventConverter();
    const d = new Float64Array(36);
    const set = (i: number, phase: number, x: number, y: number): void => {
      d.set([1000 * i, 0, phase, 7, x, y, phase === 1 || phase === 2 ? 1 : 0, 0, 0, 0, 500 + x / 2, 300 + y / 2], i * 12);
    };
    set(0, Phase.down, 20, 40);
    set(1, Phase.move, 30, 40);
    set(2, Phase.up, 30, 40);
    const events = c.expand(d, 3, 2, 0, 12);
    expect.deepEqual(events.map((e) => e.constructor.name), ["PointerAddedEvent", "PointerDownEvent", "PointerMoveEvent", "PointerUpEvent"]);
    expect.isTrue(events[0] instanceof PointerAddedEvent);
    const down = events[1] as PointerDownEvent;
    expect.deepEqual([down.position.dx, down.position.dy], [10, 20]);
    expect.deepEqual([down.screenPosition.dx, down.screenPosition.dy], [510, 320], "screen position is already in logical points");
    expect.equal((events[2] as PointerMoveEvent).delta.dx, 5);
    const again = c.expand(d, 1, 2, 0, 12);
    expect.notEqual((again[0] as PointerDownEvent).pointer, down.pointer, "new down sequence gets a new pointer id");
    d.set([0, 0, Phase.scroll, 7, 0, 0, 0, 0, 30, 0], 0);
    const s = c.expand(d, 1, 2, 0, 12)[0] as PointerScrollEvent;
    expect.equal(s.scrollDelta.dy, 15);
    void PointerUpEvent;
  });

  // 宿主的包语义: 同一序列带同一个 sequenceId;cancel 取消进行中的序列;remove 只是离开。
  test("PointerEventConverter: sequence ids, cancel and remove", () => {
    const c = new PointerEventConverter();
    const d = new Float64Array(17 * 3);
    const set = (i: number, phase: number, sequenceId: number): void => {
      d.set([0, 0, phase, 0, 10, 10, phase === Phase.down || phase === Phase.move ? 1 : 0, 0, 0, 0, 0, 0, sequenceId], i * 17);
    };
    set(0, Phase.down, 42);
    set(1, Phase.move, 42);
    set(2, Phase.cancel, 42);
    const events = c.expand(d, 3, 1);
    expect.deepEqual(events.map((e) => e.constructor.name), ["PointerAddedEvent", "PointerDownEvent", "PointerMoveEvent", "PointerCancelEvent"]);
    expect.equal(events[1]!.sequenceId, 42);
    expect.equal(c.activePointer(), null);
    set(0, Phase.hover, 0);
    set(1, Phase.remove, 0);
    const left = c.expand(d, 2, 1);
    expect.deepEqual(left.map((e) => e.constructor.name), ["PointerHoverEvent", "PointerRemovedEvent"]);
  });

  test("GestureArena: sweep picks first member; eager winner on close; hold/release", async () => {
    const arena = new GestureArenaManager();
    const log: string[] = [];
    const member = (n: string): GestureArenaMember => ({ acceptGesture: () => log.push(`${n}+`), rejectGesture: () => log.push(`${n}-`) });
    const a = member("a");
    const b = member("b");
    arena.add(1, a);
    arena.add(1, b);
    arena.close(1);
    arena.sweep(1);
    expect.deepEqual(log, ["a+", "b-"]);
    log.length = 0;
    arena.add(2, a);
    const eb = arena.add(2, b);
    eb.resolve("accepted");
    arena.close(2);
    expect.deepEqual(log, ["a-", "b+"]);
    log.length = 0;
    const ea = arena.add(3, a);
    arena.add(3, b);
    arena.close(3);
    arena.hold(3);
    arena.sweep(3);
    expect.deepEqual(log, []);
    ea.resolve("rejected");
    expect.deepEqual(log, ["a-"]);
    arena.release(3);
    expect.deepEqual(log, ["a-", "b+"]);
    log.length = 0;
    arena.add(4, a);
    arena.close(4);
    expect.deepEqual(log, [], "single member resolves in a microtask");
    await Promise.resolve();
    expect.deepEqual(log, ["a+"]);
  });

  testViews("tap vs vertical drag: small movement taps, large movement drags", async (t) => {
    const log: string[] = [];
    let dragged = 0;
    let velocity = 0;
    await t.pumpView(new Center({
      child: new GestureDetector({
        onTapDown: () => log.push("tapDown"),
        onTap: () => log.push("tap"),
        onTapCancel: () => log.push("tapCancel"),
        onVerticalDragStart: () => log.push("dragStart"),
        onVerticalDragUpdate: (d: DragUpdateDetails) => (dragged += d.primaryDelta ?? 0),
        onVerticalDragEnd: (d: DragEndDetails) => {
          log.push("dragEnd");
          velocity = d.primaryVelocity ?? 0;
        },
        child: square("g", 200),
      }),
    }));
    const target = t.one(t.byKey(new ValueKey("g")));
    await t.tap(target, { kind: Kind.touch });
    expect.deepEqual(log, ["tapDown", "tap"]);
    log.length = 0;
    await t.drag(target, new Offset(0, 80), { kind: Kind.touch, steps: 8 });
    // 超出 slop 前未到 tapDown 截止时间(100ms), 所以 tap 无任何回调
    expect.deepEqual(log, ["dragStart", "dragEnd"]);
    // dragStartBehavior=start:超出 slop(18)后的位移计入 update
    expect.isTrue(dragged > 50 && dragged <= 80, `dragged=${dragged}`);
    expect.isTrue(velocity > 100, `fling velocity ${velocity}`);
  });

  testViews("pan with mouse uses precise slop", async (t) => {
    const deltas: number[] = [];
    await t.pumpView(new Center({ child: new GestureDetector({ onPanUpdate: (d) => deltas.push(d.delta.dx), onTap: () => deltas.push(-1), child: square("p") }) }));
    await t.drag(t.one(t.byKey(new ValueKey("p"))), new Offset(10, 0), { kind: Kind.mouse, steps: 5 });
    expect.isFalse(deltas.includes(-1));
    expect.isTrue(deltas.length >= 3, `updates: ${deltas.length}`);
  });

  // 屏幕位置(跨窗口拖拽用)随拖动回调传出, 与窗口内位置独立。
  testViews("drag details carry the pointer's screen position", async (t) => {
    const screens: string[] = [];
    await t.pumpView(new Center({ child: new GestureDetector({
      onPanDown: (d) => screens.push(`down ${d.screenPosition.dx},${d.screenPosition.dy}`),
      onPanStart: (d) => screens.push(`start ${d.screenPosition.dx},${d.screenPosition.dy}`),
      onPanUpdate: (d) => screens.push(`update ${d.screenPosition.dx},${d.screenPosition.dy}`),
      child: square("s"),
    }) }));
    const p = t.centerOf(t.one(t.byKey(new ValueKey("s"))));
    t.sendPointer(Phase.down, p, { kind: Kind.mouse, screen: new Offset(1000, 500) });
    for (let i = 1; i <= 3; i++) {
      t.now += 16;
      t.sendPointer(Phase.move, p.plus(new Offset(10 * i, 0)), { kind: Kind.mouse, screen: new Offset(1000 + 10 * i, 500) });
    }
    t.sendPointer(Phase.up, p.plus(new Offset(30, 0)), { kind: Kind.mouse, buttons: 0 });
    await t.pump();
    expect.equal(screens[0], "down 1000,500");
    expect.equal(screens.at(-1), "update 1030,500");
    expect.isTrue(screens.some((l) => l.startsWith("start ")));
  });

  // ---- 指针捕获: 拖动开始即固定接收方 ----

  // 在拖动回调里捕获(典型用法): 原手势在当前事件分发完后收到取消, 之后这个指针的事件——不论来自哪个窗口——
  // 只交给捕获者, 不做命中测试、不更新悬停;松开后捕获结束, 悬停按松开处重新计算。
  testViews("capturing a pointer fixes its receiver until release", async (t, ctx) => {
    const log: string[] = [];
    const routed: string[] = [];
    let capture: PointerCapture | null = null;
    await t.pumpView(new Stack({ children: [
      new Positioned({ left: 0, top: 0, width: 100, height: 100, child: new GestureDetector({
        onPanStart: () => {
          log.push("start");
          capture = GestureBinding.instance.capturePointer((e) => routed.push(`${e.constructor.name}@${e.viewId}`));
        },
        onPanUpdate: () => log.push("update"),
        onPanEnd: () => log.push("end"),
        onPanCancel: () => log.push("cancel"),
        child: square("drag"),
      }) }),
      new Positioned({ left: 300, top: 0, width: 100, height: 100, child: new MouseRegion({
        onEnter: () => log.push("enter"),
        onExit: () => log.push("exit"),
        cursor: "click",
        child: square("hover"),
      }) }),
    ] }));
    t.sendPointer(Phase.down, new Offset(50, 50));
    t.sendPointer(Phase.move, new Offset(80, 50));  // 越过拖动阈值: onPanStart → 捕获
    expect.isTrue(capture !== null);
    const afterStart = log.length;
    expect.isTrue(log.slice(0, afterStart).includes("start"));
    expect.isTrue(log.includes("end") || log.includes("cancel"), `the original gesture lost the pointer: ${log}`);
    t.sendPointer(Phase.move, new Offset(350, 50));  // 经过 MouseRegion
    expect.deepEqual(routed, ["PointerMoveEvent@0"]);
    expect.isFalse(log.includes("update"), "the original gesture gets nothing after the capture");
    expect.isFalse(log.includes("enter"), "hover is frozen while the pointer is held");
    expect.equal(ctx.host.mock?.cursor ?? "basic", "basic");
    // 同一个指针改由另一个窗口上报(宿主改交了原生接收窗口): 照样交给捕获者。
    const d = new Float64Array(17);
    d[0] = t.now * 1000; d[2] = Phase.move; d[4] = 10; d[5] = 10; d[6] = 1;
    GestureBinding.instance.handlePointerPacket(d, 1, 1, 1);
    expect.deepEqual(routed.at(-1), "PointerMoveEvent@1");
    t.sendPointer(Phase.up, new Offset(350, 50), { buttons: 0 });
    expect.deepEqual(routed.at(-1), "PointerUpEvent@0");
    expect.isFalse(capture!.active);
    expect.isTrue(log.includes("enter"), "hover is recomputed where the pointer was released");
    await t.pump();
    // 捕获结束后照常命中测试。
    log.length = 0;
    await t.drag(new Offset(50, 50), new Offset(40, 0));
    expect.isTrue(log.includes("start"));
  }, { mockOnly: true });

  // 任何按下序列期间(不只是显式捕获)悬停都冻结: 在空白处按下拖过 MouseRegion, 不进入;松开后进入。
  testViews("hover is frozen during any press", async (t) => {
    const log: string[] = [];
    await t.pumpView(new Stack({ children: [
      new Positioned({ left: 300, top: 0, width: 100, height: 100, child: new MouseRegion({
        onEnter: () => log.push("enter"), onExit: () => log.push("exit"), child: square("hover"),
      }) }),
    ] }));
    t.sendPointer(Phase.hover, new Offset(50, 50));
    t.sendPointer(Phase.down, new Offset(50, 50));
    t.sendPointer(Phase.move, new Offset(350, 50));
    expect.deepEqual(log, []);
    t.sendPointer(Phase.up, new Offset(350, 50), { buttons: 0 });
    expect.deepEqual(log, ["enter"]);
    // 从区域内按下拖出去: 按下期间不离开, 松开后离开。
    t.sendPointer(Phase.down, new Offset(350, 50));
    t.sendPointer(Phase.move, new Offset(50, 50));
    expect.deepEqual(log, ["enter"]);
    t.sendPointer(Phase.up, new Offset(50, 50), { buttons: 0 });
    expect.deepEqual(log, ["enter", "exit"]);
    await t.pump();
  });

  testViews("capturePointer needs a pressed pointer; release drops the rest of the sequence", async (t) => {
    await t.pumpView(new Center({ child: new GestureDetector({ onTap: () => {}, child: square("c") }) }));
    expect.equal(GestureBinding.instance.capturePointer(() => {}), null, "no pointer is down");
    const routed: string[] = [];
    t.sendPointer(Phase.down, new Offset(400, 300));
    const capture = GestureBinding.instance.capturePointer((e) => routed.push(e.constructor.name))!;
    t.sendPointer(Phase.move, new Offset(410, 300));
    capture.release();
    expect.isFalse(capture.active);
    t.sendPointer(Phase.move, new Offset(420, 300));
    t.sendPointer(Phase.up, new Offset(420, 300), { buttons: 0 });
    expect.deepEqual(routed, ["PointerMoveEvent"], "events after release are dropped until the pointer is up");
    let taps = 0;
    await t.pumpView(new Center({ child: new GestureDetector({ onTap: () => taps++, child: square("c") }) }));
    await t.tap(new Offset(400, 300));
    expect.equal(taps, 1, "the next sequence is hit-tested normally");
  });

  testViews("double tap wins over tap; single tap fires after timeout", async (t) => {
    const log: string[] = [];
    await t.pumpView(new Center({ child: new GestureDetector({ onTap: () => log.push("tap"), onDoubleTap: () => log.push("double"), child: square("d") }) }));
    const target = t.one(t.byKey(new ValueKey("d")));
    await t.tap(target);
    await t.pump(50);
    await t.tap(target);
    expect.deepEqual(log, ["double"]);
    log.length = 0;
    await t.tap(target);
    expect.deepEqual(log, []);
    await t.pumpFor(400);
    expect.deepEqual(log, ["tap"]);
  });

  testViews("long press after 500ms", async (t) => {
    const log: string[] = [];
    await t.pumpView(new Center({ child: new GestureDetector({ onTap: () => log.push("tap"), onLongPress: () => log.push("long"), child: square("l") }) }));
    const p = t.centerOf(t.one(t.byKey(new ValueKey("l"))));
    t.sendPointer(Phase.down, p);
    await t.pumpFor(600);
    t.sendPointer(Phase.up, p, { buttons: 0 });
    await t.pump();
    expect.deepEqual(log, ["long"]);
  });

  testViews("secondary button tap", async (t) => {
    const log: string[] = [];
    await t.pumpView(new Center({ child: new GestureDetector({ onTap: () => log.push("primary"), onSecondaryTap: () => log.push("secondary"), child: square("s") }) }));
    await t.tap(t.one(t.byKey(new ValueKey("s"))), { buttons: 2 });
    expect.deepEqual(log, ["secondary"]);
  });

  testViews("MouseRegion enter/exit/hover and cursor", async (t) => {
    const log: string[] = [];
    await t.pumpView(new Stack({
      children: [
        new Positioned({
          left: 0, top: 0, width: 200, height: 200,
          child: new MouseRegion({
            cursor: "click",
            onEnter: () => log.push("outer+"),
            onExit: () => log.push("outer-"),
            child: new Align({
              alignment: Alignment.topLeft,
              child: new MouseRegion({ cursor: "text", onEnter: () => log.push("inner+"), onExit: () => log.push("inner-"), onHover: (e) => log.push(`h${e.localPosition.dx}`), child: square("inner", 50) }),
            }),
          }),
        }),
      ],
    }));
    await t.hover(new Offset(100, 100));
    expect.deepEqual(log, ["outer+"]);
    expect.equal(t.binding.mouseTracker.currentCursor, "click");
    await t.hover(new Offset(10, 10));
    // 同 Flutter: 先进入区域, 再收到该位置的悬停(MouseTracker 在分发之前更新)。
    expect.deepEqual(log, ["outer+", "inner+", "h10"]);
    expect.equal(t.binding.mouseTracker.currentCursor, "text");
    await t.hover(new Offset(300, 300));
    expect.deepEqual(log.slice(3), ["inner-", "outer-"]);
    expect.equal(t.binding.mouseTracker.currentCursor, "basic");
  });

  testViews("cursor is sent to window.setCursor", async (t, ctx) => {
    await t.pumpView(new Column({ children: [new MouseRegion({ cursor: "resizeLeftRight", child: square("c") })] }));
    await t.hover(new Offset(400, 50));
    expect.equal(ctx.host.mock!.cursor, "resizeLeftRight");
    await t.hover(new Offset(500, 500));
    expect.equal(ctx.host.mock!.cursor, "basic");
  }, { mockOnly: true });
});
