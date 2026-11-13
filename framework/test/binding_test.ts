import {
  app, Center, SchedulerBinding, SizedBox, StandardMethodCodec, MethodCall, Text, ViewsBinding, defaultBinaryMessenger,
  Size, MediaQuery, Builder, type View,
} from "../packages/momo/src/index";
import { expect, group } from "./harness";
import { testViews } from "./util";

group("binding", () => {
  testViews("idle app requests no frames; setState requests exactly one", async (t, ctx) => {
    await t.pumpView(new Center({ child: new SizedBox({ width: 10, height: 10 }) }));
    await t.pump();
    expect.isFalse(SchedulerBinding.instance.hasScheduledFrame);
    const before = ctx.host.mock!.frameRequests;
    t.binding.renderView.markNeedsPaint();
    t.binding.renderView.markNeedsPaint();
    expect.equal(ctx.host.mock!.frameRequests - before, 1);
    await t.pump();
  }, { mockOnly: true });

  testViews("app state: suspended (app hidden) disables frames, running re-enables", async (t) => {
    await t.pumpView(new Text("x"));
    const state = (s: string): ArrayBuffer =>
      StandardMethodCodec.instance.encodeMethodCall(new MethodCall("stateChanged", { state: s }));
    await defaultBinaryMessenger.handlePlatformMessage("momo/app", state("suspended"));
    expect.equal(app.state, "suspended");
    expect.isFalse(SchedulerBinding.instance.framesEnabled);
    await defaultBinaryMessenger.handlePlatformMessage("momo/app", state("running"));
    expect.isTrue(SchedulerBinding.instance.framesEnabled);
  });

  testViews("metrics change rebuilds MediaQuery dependents", async (t) => {
    const sizes: string[] = [];
    const w: View = new Builder({ builder: (c) => {
      sizes.push(MediaQuery.of(c).size.toString());
      return new SizedBox();
    } });
    await t.pumpView(w);
    t.setViewSize(new Size(400, 300), 2);
    await t.pump();
    expect.deepEqual(sizes, [new Size(800, 600).toString(), new Size(400, 300).toString()]);
    expect.equal(ViewsBinding.instance.viewConfiguration.devicePixelRatio, 2);
  });
});
