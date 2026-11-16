import {
  app, Center, SchedulerBinding, SizedBox, StandardMethodCodec, MethodCall, Text, defaultBinaryMessenger,
  Size, MediaQuery, Builder, type View,
} from "../packages/momo/src/index";
import { fakeClock, hostTimerHost } from "./fake_clock";
import { expect, group } from "./harness";
import { testViews } from "./util";

group("binding", () => {
  testViews("idle app requests no frames; setState requests exactly one", async (t, ctx) => {
    await t.pumpView(new Center({ child: new SizedBox({ width: 10, height: 10 }) }));
    await t.pump();
    expect.isFalse(SchedulerBinding.instance.hasScheduledFrame);
    const before = ctx.host.mock!.frameRequests;
    t.renderView.markNeedsPaint();
    t.renderView.markNeedsPaint();
    expect.equal(ctx.host.mock!.frameRequests - before, 1);
    await t.pump();
  }, { mockOnly: true });

  // 延迟任务由定时器驱动(测试里是假时钟): 等待期间不请求帧。
  testViews("delayed tasks run on a timer and request no frames while waiting", async (t) => {
    await t.pumpView(new SizedBox());
    await t.pump();
    const log: string[] = [];
    const scheduler = SchedulerBinding.instance;
    const first = scheduler.scheduleDelayed(100, () => log.push("100"));
    scheduler.scheduleDelayed(30, () => {
      log.push("30");
      scheduler.scheduleDelayed(30, () => log.push("60"));
    });
    const cancelled = scheduler.scheduleDelayed(40, () => log.push("cancelled"));
    cancelled.cancel();
    expect.isFalse(cancelled.isActive);
    expect.isFalse(scheduler.hasScheduledFrame, "waiting does not request frames");
    await t.pump(20);
    expect.deepEqual(log, []);
    expect.isFalse(scheduler.hasScheduledFrame);
    await t.pump(50);  // 30 到期;它登记的任务(60)也在这段时间内到期
    expect.deepEqual(log, ["30", "60"]);
    expect.isTrue(first.isActive);
    await t.pump(30);
    expect.deepEqual(log, ["30", "60", "100"]);
    expect.isFalse(first.isActive);
    expect.isFalse(scheduler.hasScheduledFrame);
  });

  // 生产路径: 延迟任务由宿主定时器(mock 宿主下是 Node 的、真实引擎下是 libnode 的 setTimeout)驱动——没有帧
  // 也会到期。上面的用例用假时钟验证语义, 这里换回真实定时器验证它确实触发、取消确实生效。
  testViews("delayed tasks fire on the real host timer without any frame", async (t) => {
    await t.pumpView(new SizedBox());
    await t.pump();
    const scheduler = SchedulerBinding.instance;
    scheduler.timerHost = hostTimerHost;
    try {
      const log: string[] = [];
      const fired = new Promise<void>((resolve) => {
        scheduler.scheduleDelayed(20, () => {
          log.push("fired");
          resolve();
        });
      });
      const cancelled = scheduler.scheduleDelayed(5, () => log.push("cancelled"));
      cancelled.cancel();
      const frames = scheduler.frameCount;
      expect.isFalse(scheduler.hasScheduledFrame, "waiting does not request frames");
      // 不出帧、不推进假时钟: 只有真实的定时器能让它到期(到不了的话整个测试超时)。
      await fired;
      expect.deepEqual(log, ["fired"], "the cancelled task never runs");
      expect.equal(scheduler.frameCount, frames, "no frame was needed");
      expect.equal(fakeClock.pendingTimers, 0, "the fake clock was not involved");
    } finally {
      scheduler.timerHost = fakeClock;
    }
  });

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
    expect.equal(t.renderView.configuration.devicePixelRatio, 2);
  });
});
