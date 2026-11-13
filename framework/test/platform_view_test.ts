// 平台视图(docs/05 §9): PlatformView view ↔ momo/platform_views 通道 ↔ SceneBuilder.addPlatformView。
import {
  ColoredBox, Colors, Focus, IgnorePointer, GestureDetector, FocusManager, FocusNode, MethodCall, PlatformView, PlatformViewLayer, PlatformViewsService,
  Positioned, SizedBox, Stack, StandardMethodCodec, defaultBinaryMessenger, type PlatformViewController,
} from "../packages/momo/src/index";
import { expect, group } from "./harness";
import type { RecordedOp } from "./host";
import { Offset } from "../packages/momo/src/index";

import { testViews } from "./util";

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
  testViews("PlatformView creates the view and adds a platform view layer below later content", async (t, ctx) => {
    const calls = mockPlatformViews();
    try {
      let created: PlatformViewController | null = null;
      const params = new Uint8Array([1, 2, 3]);
      await t.pumpView(new Stack({
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

      // 移除 view → dispose。
      await t.pumpView(new SizedBox({}));
      await t.pump();
      expect.equal(calls.at(-1)!.method, "dispose");
      expect.equal(calls.at(-1)!.args.id, id);
      expect.isFalse(ctx.host.mock!.lastSceneOps.some((o) => o.op === "addPlatformView"));
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testViews("create failure reports through onCreateError and keeps the placeholder", async (t, ctx) => {
    const calls = mockPlatformViews(["missing.view"]);
    try {
      let error: unknown = null;
      await t.pumpView(new SizedBox({ width: 100, height: 100,
        child: new PlatformView({ viewType: "missing.view", onCreateError: (e) => { error = e; } }) }));
      await t.pump();
      expect.equal(calls.length, 1);
      expect.isTrue(error instanceof Error && String((error as Error).message).includes("missing.view"), String(error));
      expect.isFalse(ctx.host.mock!.lastSceneOps.some((o) => o.op === "addPlatformView"));
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testViews("focus follows the native view and focusing the view focuses the native view", async (t) => {
    const calls = mockPlatformViews();
    try {
      const node = new FocusNode({ debugLabel: "web" });
      const other = new FocusNode({ debugLabel: "other" });
      await t.pumpView(new Stack({
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
      await t.pumpView(new SizedBox({}));  // 卸载(dispose 仍走 mock)
      await t.pump();
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  // ---- 输入(docs/05 §9 "输入"): 宿主在主线程按每帧几何直接把原生事件交给原生视图, 不经过框架。
  // 框架里的平台视图只占位: 几何刚变化、新帧尚未呈现时落到这里的事件不穿透到下面的 momo 内容。
  // 宿主按"平台视图之上的可命中区域"决定原生事件归谁: 只记录自身可命中的节点, IgnorePointer 子树与平台视图本身不记。
  testViews("content above a platform view records hit regions; ignored subtrees do not", async (t, ctx) => {
    mockPlatformViews();
    try {
      const build = (ignoring: boolean) => new Stack({
        children: [
          new Positioned({ left: 0, top: 0, width: 300, height: 200, child: new PlatformView({ viewType: "test.view" }) }),
          new Positioned({ left: 10, top: 10, width: 40, height: 40, child: new ColoredBox({ color: Colors.red }) }),
          new Positioned({ left: 100, top: 10, width: 40, height: 40,
            child: new IgnorePointer({ ignoring, child: new ColoredBox({ color: Colors.blue }) }) }),
        ],
      });
      const hitRegions = (): unknown[][] => ctx.host.mock!.lastSceneOps
        .filter((o) => o.op === "addPicture")
        .flatMap((o) => ctx.host.mock!.pictureOps(o.args[2]))
        .filter((o) => o.op === "addHitRegion")
        .map((o) => o.args);
      await t.pumpView(build(true));
      await t.pump();
      expect.deepEqual(hitRegions(), [[10, 10, 40, 40]]);
      await t.pumpView(build(false));
      await t.pump();
      expect.deepEqual(hitRegions(), [[10, 10, 40, 40], [100, 10, 40, 40]]);
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testViews("a platform view absorbs pointer events without forwarding anything", async (t) => {
    const calls = mockPlatformViews();
    try {
      let below = 0;
      await t.pumpView(new Stack({
        children: [
          Positioned.fill({ child: new GestureDetector({ onTap: () => below++, child: new ColoredBox({ color: Colors.white }) }) }),
          new Positioned({ left: 0, top: 0, width: 300, height: 200, child: new PlatformView({ viewType: "test.view" }) }),
        ],
      }));
      await t.pump();
      await t.tap(new Offset(150, 100));
      expect.equal(below, 0, "content under the platform view is not hit");
      await t.tap(new Offset(500, 400));
      expect.equal(below, 1);
      expect.deepEqual(calls.map((c) => c.method), ["create"], "nothing but the view's creation reaches the host");
      await t.pumpView(new SizedBox({}));
      await t.pump();
    } finally {
      cleanup();
    }
  }, { mockOnly: true });

  testViews("view ids are unique and positive", async () => {
    const a = PlatformViewsService.allocateId();
    const b = PlatformViewsService.allocateId();
    expect.isTrue(a > 0 && b > 0 && a !== b && Number.isSafeInteger(b));
  }, { mockOnly: true });
});
