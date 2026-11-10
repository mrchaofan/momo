// 平台视图(docs/05 §9): PlatformView widget ↔ momo/platform_views 通道 ↔ SceneBuilder.addPlatformView。
import {
  ColoredBox, Colors, Focus, GestureDetector, FocusManager, FocusNode, MethodCall, PlatformView, PlatformViewLayer, PlatformViewsService,
  Positioned, SizedBox, Stack, StandardMethodCodec, defaultBinaryMessenger, type PlatformViewController,
} from "../packages/momo/src/index";
import { expect, group } from "./harness";
import type { RecordedOp } from "./host";
import { Offset } from "../packages/momo/src/index";
import { Kind, Phase, type WidgetTester } from "./tester";
import { testWidgets } from "./util";

const CHANNEL = "momo/platform_views";
const codec = StandardMethodCodec.instance;

interface Recorded { method: string; args: Record<string, unknown> }

/** 拦截 momo/platform_views: 记录调用;failTypes 中的类型 create 返回错误信封。 */
function mockPlatformViews(failTypes: string[] = []): Recorded[] {
  const calls: Recorded[] = [];
  defaultBinaryMessenger.setMockMessageHandler(CHANNEL, (data) => {
    const call: MethodCall = codec.decodeMethodCall(data);
    const args = (call.args ?? {}) as Record<string, unknown>;
    calls.push({ method: call.method, args });
    if (call.method === "create" && failTypes.includes(String(args.viewType))) {
      return codec.encodeErrorEnvelope("error", `no platform view factory registered for type '${String(args.viewType)}'`);
    }
    return codec.encodeSuccessEnvelope(null);
  });
  return calls;
}

function cleanup(): void {
  defaultBinaryMessenger.setMockMessageHandler(CHANNEL, null);
}

group("platform views", () => {
  testWidgets("PlatformView creates the view and adds a platform view layer below later content", async (t, ctx) => {
    const calls = mockPlatformViews();
    try {
      let created: PlatformViewController | null = null;
      const params = new Uint8Array([1, 2, 3]);
      await t.pumpWidget(new Stack({
        children: [
          new Positioned({ left: 10, top: 20, width: 300, height: 200,
            child: new PlatformView({ viewType: "test.view", creationParams: params, onCreated: (c) => { created = c; } }) }),
          // 绘制顺序在平台视图之后: 应叠在它上面。
          new Positioned({ left: 50, top: 50, width: 40, height: 40, child: new ColoredBox({ color: Colors.red }) }),
        ],
      }));
      await t.pump();
      expect.equal(calls.length, 1);
      expect.equal(calls[0]!.method, "create");
      expect.equal(calls[0]!.args.viewType, "test.view");
      expect.deepEqual(Array.from(calls[0]!.args.params as Uint8Array), [1, 2, 3]);
      const id = calls[0]!.args.id as number;
      expect.isTrue(Number.isSafeInteger(id) && id > 0, `id ${id}`);
      expect.isTrue(created !== null);
      expect.equal(created!.viewId, id);

      const ops: RecordedOp[] = ctx.host.mock!.lastSceneOps;
      const pv = ops.findIndex((o) => o.op === "addPlatformView");
      expect.isTrue(pv >= 0, ops.map((o) => o.op).join(","));
      expect.deepEqual(ops[pv]!.args, [id, 10, 20, 300, 200]);
      // 红色方块的 picture 在平台视图之后加入场景(合成时进入视图之上的 overlay)。
      expect.isTrue(ops.slice(pv + 1).some((o) => o.op === "addPicture"), "content painted after the view");
      expect.isTrue(t.binding.renderView.rootLayer !== null);
      void PlatformViewLayer;

      // 移除 widget → dispose。
      await t.pumpWidget(new SizedBox({}));
      await t.pump();
      expect.equal(calls.at(-1)!.method, "dispose");
      expect.equal(calls.at(-1)!.args.id, id);
      expect.isFalse(ctx.host.mock!.lastSceneOps.some((o) => o.op === "addPlatformView"));
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testWidgets("create failure reports through onCreateError and keeps the placeholder", async (t, ctx) => {
    const calls = mockPlatformViews(["missing.view"]);
    try {
      let error: unknown = null;
      await t.pumpWidget(new SizedBox({ width: 100, height: 100,
        child: new PlatformView({ viewType: "missing.view", onCreateError: (e) => { error = e; } }) }));
      await t.pump();
      expect.equal(calls.length, 1);
      expect.isTrue(error instanceof Error && String((error as Error).message).includes("missing.view"), String(error));
      expect.isFalse(ctx.host.mock!.lastSceneOps.some((o) => o.op === "addPlatformView"));
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testWidgets("focus follows the native view and focusing the widget focuses the native view", async (t) => {
    const calls = mockPlatformViews();
    try {
      const node = new FocusNode({ debugLabel: "web" });
      const other = new FocusNode({ debugLabel: "other" });
      await t.pumpWidget(new Stack({
        children: [
          new Positioned({ left: 0, top: 0, width: 100, height: 100, child: new PlatformView({ viewType: "test.view", focusNode: node }) }),
          new Positioned({ left: 0, top: 120, width: 10, height: 10, child: new Focus({ focusNode: other, child: new SizedBox({}) }) }),
        ],
      }));
      await t.pump();
      const id = calls[0]!.args.id as number;

      other.requestFocus();
      await t.pump();
      expect.equal(FocusManager.instance.primaryFocus, other);

      // 原生视图被点击获得焦点 → momo 焦点移到 PlatformView, 且不回发 focus。
      PlatformViewsService.debugNotifyFocused(id);
      await t.pump();
      expect.equal(FocusManager.instance.primaryFocus, node);
      expect.isFalse(calls.some((c) => c.method === "focus"));

      // 在 momo 中把焦点交给 PlatformView → 通知原生侧。
      other.requestFocus();
      await t.pump();
      node.requestFocus();
      await t.pump();
      const focus = calls.filter((c) => c.method === "focus");
      expect.equal(focus.length, 1);
      expect.equal(focus[0]!.args.id, id);
      await t.pumpWidget(new SizedBox({}));  // 卸载(dispose 仍走 mock)
      await t.pump();
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  // ---- 输入(docs/05 §9 "输入"): 框架命中测试决定接收者, 经 view.dispatchPlatformViewPointer 交给宿主 ----

  /** 网页(平台视图)占 (0,0)-(300,200), 其上 (0,0)-(100,50) 盖着一块 momo 内容(可命中)。 */
  async function pumpWebWithOverlay(t: WidgetTester, calls: Recorded[]): Promise<number> {
    await t.pumpWidget(new Stack({
      children: [
        new Positioned({ left: 0, top: 0, width: 300, height: 200, child: new PlatformView({ viewType: "test.view" }) }),
        new Positioned({ left: 0, top: 0, width: 100, height: 50,
          child: new GestureDetector({ onTap: () => {}, child: new ColoredBox({ color: Colors.red }) }) }),
      ],
    }));
    await t.pump();
    return calls[0]!.args.id as number;
  }

  const EVENT = 0, ENTER = 1, EXIT = 2;

  testWidgets("pointer events hit-tested onto a platform view are forwarded with their native ids", async (t, ctx) => {
    const calls = mockPlatformViews();
    try {
      const id = await pumpWebWithOverlay(t, calls);
      const log = ctx.host.mock!.platformViewPointer;
      log.length = 0;
      // 悬停在网页上: 先进入再转发移动(不要求网页聚焦);进入带触发它的原生事件 id。
      t.sendPointer(Phase.hover, new Offset(200, 100), { nativeEventId: 1 });
      expect.deepEqual(log, [
        { view: 0, platformViewId: id, nativeEventId: 1, action: ENTER },
        { view: 0, platformViewId: id, nativeEventId: 1, action: EVENT },
      ]);
      // 移到盖在网页上的 momo 内容: 离开, 不转发(命中测试即遮挡判断)。
      log.length = 0;
      t.sendPointer(Phase.hover, new Offset(50, 25), { nativeEventId: 2 });
      expect.deepEqual(log, [{ view: 0, platformViewId: id, nativeEventId: 2, action: EXIT }]);
      // 合成事件(id 0)不转发。
      log.length = 0;
      t.sendPointer(Phase.hover, new Offset(200, 100));
      expect.deepEqual(log.filter((e) => e.action === EVENT), []);
      await t.pumpWidget(new SizedBox({}));
      await t.pump();
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testWidgets("a press on the platform view captures the sequence; a press on momo content returns focus", async (t, ctx) => {
    const calls = mockPlatformViews();
    try {
      const id = await pumpWebWithOverlay(t, calls);
      const log = ctx.host.mock!.platformViewPointer;
      log.length = 0;
      // 在网页上按下, 拖到 momo 内容上再抬起: 整个序列都交给网页(按下时的命中路径即捕获)。
      t.sendPointer(Phase.down, new Offset(200, 100), { nativeEventId: 10 });
      t.sendPointer(Phase.move, new Offset(50, 25), { nativeEventId: 11 });
      t.sendPointer(Phase.up, new Offset(50, 25), { nativeEventId: 12, buttons: 0 });
      expect.deepEqual(log.filter((e) => e.action === EVENT).map((e) => [e.platformViewId, e.nativeEventId]),
        [[id, 10], [id, 11], [id, 12]]);
      // 在 momo 内容上按下: 宿主收到"由 momo 消费"(平台视图 0), 据此把键盘焦点交还 momo。
      log.length = 0;
      t.sendPointer(Phase.down, new Offset(50, 25), { nativeEventId: 13 });
      t.sendPointer(Phase.up, new Offset(50, 25), { nativeEventId: 14, buttons: 0 });
      expect.deepEqual(log.filter((e) => e.action === EVENT).map((e) => [e.platformViewId, e.nativeEventId]), [[0, 13]]);
      await t.pumpWidget(new SizedBox({}));
      await t.pump();
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testWidgets("scrolling over a platform view goes to it, latched for a trackpad gesture", async (t, ctx) => {
    const calls = mockPlatformViews();
    try {
      const id = await pumpWebWithOverlay(t, calls);
      const log = ctx.host.mock!.platformViewPointer;
      log.length = 0;
      const trackpad = { kind: Kind.trackpad, buttons: 0 };
      // 手势在网页上开始(零位移的开始事件也转发), 中途指针移到 momo 内容上: 仍归网页, 直到结束。
      t.sendPointer(Phase.scroll, new Offset(200, 100), { ...trackpad, nativeEventId: 20, scrollPhase: 1 });
      t.sendPointer(Phase.scroll, new Offset(50, 25), { ...trackpad, nativeEventId: 21, scrollPhase: 2, scrollDeltaY: 10 });
      t.sendPointer(Phase.scroll, new Offset(50, 25), { ...trackpad, nativeEventId: 22, scrollPhase: 3 });
      // 手势结束后重新命中测试: momo 内容上的滚轮不归网页。
      t.sendPointer(Phase.scroll, new Offset(50, 25), { buttons: 0, nativeEventId: 23, scrollDeltaY: 10 });
      expect.deepEqual(log.filter((e) => e.action === EVENT).map((e) => [e.platformViewId, e.nativeEventId]),
        [[id, 20], [id, 21], [id, 22]]);
      await t.pumpWidget(new SizedBox({}));
      await t.pump();
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testWidgets("trackpad gestures and force touch take the same route: framework first, then the native view", async (t, ctx) => {
    const calls = mockPlatformViews();
    try {
      const id = await pumpWebWithOverlay(t, calls);
      const log = ctx.host.mock!.platformViewPointer;
      const g = { kind: Kind.trackpad, buttons: 0 };
      const MAGNIFY = 1, SMART_MAGNIFY = 3, PRESSURE = 6;
      log.length = 0;
      // 捏合在网页上开始, 中途指针下变成 momo 内容: 整个手势仍归网页(按阶段锁定)。
      t.sendPointer(Phase.gesture, new Offset(200, 100), { ...g, nativeEventId: 50, gestureKind: MAGNIFY, scrollPhase: 1 });
      t.sendPointer(Phase.gesture, new Offset(50, 25), { ...g, nativeEventId: 51, gestureKind: MAGNIFY, scrollPhase: 2, gestureValue: 0.1 });
      t.sendPointer(Phase.gesture, new Offset(50, 25), { ...g, nativeEventId: 52, gestureKind: MAGNIFY, scrollPhase: 3 });
      // 盖在网页上的 momo 内容处的智能缩放: 命中 momo 内容, 不交给网页。
      t.sendPointer(Phase.gesture, new Offset(50, 25), { ...g, nativeEventId: 53, gestureKind: SMART_MAGNIFY });
      // 网页上的智能缩放: 交给网页。
      t.sendPointer(Phase.gesture, new Offset(200, 100), { ...g, nativeEventId: 54, gestureKind: SMART_MAGNIFY });
      expect.deepEqual(log.filter((e) => e.action === EVENT).map((e) => [e.platformViewId, e.nativeEventId]),
        [[id, 50], [id, 51], [id, 52], [id, 54]]);
      // Force Touch 压力属于按下序列: 在网页上按下 → 压力交给网页, 即使指针已移到 momo 内容上。
      log.length = 0;
      t.sendPointer(Phase.down, new Offset(200, 100), { nativeEventId: 60 });
      t.sendPointer(Phase.gesture, new Offset(50, 25), { nativeEventId: 61, gestureKind: PRESSURE, gestureValue: 0.7, gestureStage: 2 });
      t.sendPointer(Phase.up, new Offset(50, 25), { nativeEventId: 62, buttons: 0 });
      // 在 momo 内容上按下: 压力不交给网页。
      t.sendPointer(Phase.down, new Offset(50, 25), { nativeEventId: 63 });
      t.sendPointer(Phase.gesture, new Offset(50, 25), { nativeEventId: 64, gestureKind: PRESSURE, gestureValue: 0.7, gestureStage: 2 });
      t.sendPointer(Phase.up, new Offset(50, 25), { nativeEventId: 65, buttons: 0 });
      expect.deepEqual(log.filter((e) => e.action === EVENT).map((e) => [e.platformViewId, e.nativeEventId]),
        [[id, 60], [id, 61], [id, 62], [0, 63]]);
      await t.pumpWidget(new SizedBox({}));
      await t.pump();
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testWidgets("leaving the window from a platform view: the exit follows every forwarded move, in order", async (t, ctx) => {
    const calls = mockPlatformViews();
    try {
      const id = await pumpWebWithOverlay(t, calls);
      const log = ctx.host.mock!.platformViewPointer;
      log.length = 0;
      t.sendPointer(Phase.hover, new Offset(200, 100), { nativeEventId: 40 });
      expect.isTrue(log.every((e) => e.platformViewId === id));
      t.sendPointer(Phase.hover, new Offset(290, 100), { nativeEventId: 41 });
      // 宿主: 指针离开窗口 → remove(带离开事件的 id, 位置在窗口外)。
      t.sendPointer(Phase.remove, new Offset(310, 100), { nativeEventId: 42, buttons: 0 });
      expect.deepEqual(log.map((e) => [e.action, e.nativeEventId]), [[ENTER, 40], [EVENT, 40], [EVENT, 41], [EXIT, 42]],
        "the host sees enter, moves and exit in the order the framework handled them");
      await t.pumpWidget(new SizedBox({}));
      await t.pump();
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testWidgets("removing a hovered platform view releases pointer ownership", async (t, ctx) => {
    const calls = mockPlatformViews();
    try {
      const id = await pumpWebWithOverlay(t, calls);
      const log = ctx.host.mock!.platformViewPointer;
      t.sendPointer(Phase.hover, new Offset(200, 100), { nativeEventId: 30 });
      log.length = 0;
      await t.pumpWidget(new SizedBox({}));
      await t.pump();
      expect.deepEqual(log, [{ view: 0, platformViewId: id, nativeEventId: 0, action: EXIT }]);
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testWidgets("view ids are unique and positive", async () => {
    const a = PlatformViewsService.allocateId();
    const b = PlatformViewsService.allocateId();
    expect.isTrue(a > 0 && b > 0 && a !== b && Number.isSafeInteger(b));
  }, { mockOnly: true });
});
