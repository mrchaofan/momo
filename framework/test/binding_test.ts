import {
  AppLifecycle, Center, SchedulerBinding, SizedBox, StandardMessageCodec, Text, WidgetsBinding, defaultBinaryMessenger,
  Size, MediaQuery, Builder, type Widget,
} from "../packages/momo/src/index";
import { expect, group } from "./harness";
import { testWidgets } from "./util";

group("binding", () => {
  testWidgets("idle app requests no frames; setState requests exactly one", async (t, ctx) => {
    await t.pumpWidget(new Center({ child: new SizedBox({ width: 10, height: 10 }) }));
    await t.pump();
    expect.isFalse(SchedulerBinding.instance.hasScheduledFrame);
    const before = ctx.host.mock!.frameRequests;
    t.binding.renderView.markNeedsPaint();
    t.binding.renderView.markNeedsPaint();
    expect.equal(ctx.host.mock!.frameRequests - before, 1);
    await t.pump();
  }, { mockOnly: true });

  testWidgets("lifecycle: suspended disables frames, resumed re-enables", async (t) => {
    await t.pumpWidget(new Text("x"));
    const enc = (s: string): ArrayBuffer | null => StandardMessageCodec.instance.encodeMessage(s);
    await defaultBinaryMessenger.handlePlatformMessage("momo/lifecycle", enc("suspended"));
    expect.equal(AppLifecycle.instance.state, "suspended");
    expect.isFalse(SchedulerBinding.instance.framesEnabled);
    await defaultBinaryMessenger.handlePlatformMessage("momo/lifecycle", enc("resumed"));
    expect.isTrue(SchedulerBinding.instance.framesEnabled);
  });

  testWidgets("metrics change rebuilds MediaQuery dependents", async (t) => {
    const sizes: string[] = [];
    const w: Widget = new Builder({ builder: (c) => {
      sizes.push(MediaQuery.of(c).size.toString());
      return new SizedBox();
    } });
    await t.pumpWidget(w);
    t.setViewSize(new Size(400, 300), 2);
    await t.pump();
    expect.deepEqual(sizes, [new Size(800, 600).toString(), new Size(400, 300).toString()]);
    expect.equal(WidgetsBinding.instance.viewConfiguration.devicePixelRatio, 2);
  });
});
