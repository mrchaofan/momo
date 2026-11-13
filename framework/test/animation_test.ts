import {
  AnimationController,
  ColorTween,
  CurveTween,
  CurvedAnimation,
  Curves,
  FrictionSimulation,
  IntTween,
  Interval,
  ReverseAnimation,
  SpringDescription,
  SpringSimulation,
  Tween,
  type AnimationStatus,
} from "../packages/momo/src/animation/index";
import { Color } from "../packages/momo/src/painting/color";
import { SchedulerBinding } from "../packages/momo/src/scheduler/index";
import { State, type BuildContext, type View } from "../packages/momo/src/views/framework";
import { expect, group, test as rawTest, type TestContext } from "./harness";

let now = 100000;

/** 推进假时钟;有帧请求时执行一帧。 */
function pump(ms = 16): void {
  now += ms;
  const b = SchedulerBinding.instance;
  if (b.hasScheduledFrame) {
    b.handleBeginFrame(now);
    b.handleDrawFrame();
  }
}

function pumpFor(totalMs: number, step = 16): void {
  for (let t = 0; t < totalMs; t += step) pump(step);
}

/** 把挂起的帧回调执行掉, 并清空残留。 */
function flushAndReset(): void {
  pump(0);
  SchedulerBinding.instance.debugResetForTest();
}

/** 每个测试结束后清理调度器(仅作用于本文件的测试)。 */
function test(name: string, body: (ctx: TestContext) => void | Promise<void>): void {
  rawTest(name, async (ctx) => {
    try {
      await body(ctx);
    } finally {
      flushAndReset();
    }
  });
}

const microtasks = (): Promise<void> => new Promise<void>((r) => r()).then(() => undefined);

group("animation", () => {
  test("forward runs to completion with exact end value and status sequence", () => {
    const c = new AnimationController({ duration: 100 });
    const statuses: AnimationStatus[] = [];
    const values: number[] = [];
    c.addStatusListener((s) => statuses.push(s));
    c.addListener(() => values.push(c.value));
    expect.equal(c.status, "dismissed");
    c.forward();
    expect.equal(c.status, "forward");
    expect.isTrue(c.isAnimating);
    pump(); // 首帧 elapsed = 0
    expect.equal(c.value, 0);
    pump(); // 16ms
    expect.close(c.value, 0.16, 1e-9);
    pumpFor(200);
    expect.equal(c.value, 1);
    expect.equal(c.status, "completed");
    expect.isFalse(c.isAnimating);
    expect.deepEqual(statuses, ["forward", "completed"]);
    for (let i = 1; i < values.length; i++) expect.isTrue(values[i]! >= values[i - 1]!, "monotonic");
    c.dispose();
  });

  test("reverse from upper bound reaches dismissed", () => {
    const c = new AnimationController({ duration: 100, reverseDuration: 50, value: 1 });
    expect.equal(c.status, "completed");
    const statuses: AnimationStatus[] = [];
    c.addStatusListener((s) => statuses.push(s));
    c.reverse();
    pump();
    pump();
    expect.close(c.value, 1 - 16 / 50, 1e-9);
    pumpFor(100);
    expect.equal(c.value, 0);
    expect.deepEqual(statuses, ["reverse", "dismissed"]);
    c.dispose();
  });

  test("animateTo with curve and explicit duration", () => {
    const c = new AnimationController({ duration: 1000 });
    c.animateTo(0.5, { duration: 100, curve: Curves.easeIn });
    pump();
    pump(50);
    const expected = 0.5 * Curves.easeIn.transform(0.5);
    expect.close(c.value, expected, 1e-9);
    pumpFor(100);
    expect.equal(c.value, 0.5);
    expect.equal(c.status, "completed"); // 正向结束即 completed(同 Flutter, 即使停在中间值)
    expect.isFalse(c.isAnimating);
    c.dispose();
  });

  test("animateTo current value with zero duration completes synchronously", async () => {
    const c = new AnimationController({ duration: 100 });
    const f = c.animateTo(0, { duration: 0 });
    expect.isTrue(f.isCompleted);
    expect.equal(c.status, "completed");
    await f;
    c.dispose();
  });

  test("repeat(reverse) ping-pongs across cycles", () => {
    const c = new AnimationController({ duration: 100 });
    c.repeat({ reverse: true });
    pump(); // t=0
    pump(50);
    expect.close(c.value, 0.5, 1e-9);
    expect.equal(c.status, "forward");
    pump(100); // t=150 → 反向段中点
    expect.close(c.value, 0.5, 1e-9);
    expect.equal(c.status, "reverse");
    pump(75); // t=225 → 正向 0.25
    expect.close(c.value, 0.25, 1e-9);
    expect.equal(c.status, "forward");
    expect.isTrue(c.isAnimating);
    c.stop();
    expect.isFalse(c.isAnimating);
    c.dispose();
  });

  test("TickerFuture completes on finish; orCancel rejects on stop", async () => {
    const c = new AnimationController({ duration: 50 });
    let done = false;
    void c.forward().then(() => {
      done = true;
    });
    pumpFor(100);
    await microtasks();
    expect.isTrue(done);

    const f = c.reverse();
    const cancelled = f.orCancel;
    pump();
    c.stop();
    const err = await expect.rejects(cancelled);
    expect.equal((err as Error).name, "TickerCanceled");
    let resolved = false;
    void f.then(() => {
      resolved = true;
    });
    await microtasks();
    expect.isFalse(resolved, "canceled future never completes");
    c.dispose();
  });

  test("State releases tickers disposed individually (no accumulation)", () => {
    class VsyncState extends State {
      build(_context: BuildContext): View {
        throw new Error("not built");
      }
    }
    const s = new VsyncState();
    for (let i = 0; i < 100; i++) {
      const c = new AnimationController({ duration: 50, vsync: s });
      c.forward();
      pump();
      c.dispose();
    }
    expect.equal(s.debugTickerCount, 0);

    // resync: 旧 provider 的 Ticker 被 absorb 后释放, 也应移出。
    const s2 = new VsyncState();
    const c = new AnimationController({ duration: 100, vsync: s });
    c.forward();
    pump();
    c.resync(s2);
    expect.equal(s.debugTickerCount, 0);
    expect.equal(s2.debugTickerCount, 1);
    expect.isTrue(c.isAnimating);
    pump(30);
    expect.isTrue(c.value > 0);
    s2._disposeTickers();
    expect.equal(s2.debugTickerCount, 0);
    expect.isFalse(c.isAnimating);
    c.dispose(); // 重复释放幂等
  });

  test("value setter stops animation and notifies", () => {
    const c = new AnimationController({ duration: 100 });
    let notified = 0;
    c.addListener(() => notified++);
    c.forward();
    pump();
    c.value = 0.3;
    expect.isFalse(c.isAnimating);
    expect.equal(c.value, 0.3);
    expect.equal(notified, 2);
    c.value = 5; // clamp
    expect.equal(c.value, 1);
    expect.equal(c.status, "completed");
    c.dispose();
  });

  test("CurvedAnimation uses reverseCurve while reversing", () => {
    const c = new AnimationController({ duration: 100 });
    const curved = new CurvedAnimation({ parent: c, curve: Curves.easeIn, reverseCurve: Curves.easeOut });
    c.value = 0.5;
    expect.close(curved.value, Curves.easeIn.transform(0.5), 1e-12);
    c.forward();
    pump();
    pumpFor(200);
    expect.equal(curved.value, 1);
    c.reverse();
    pump();
    pump(50);
    expect.close(c.value, 0.5, 1e-9);
    expect.close(curved.value, Curves.easeOut.transform(0.5), 1e-12);
    curved.dispose();
    c.dispose();
  });

  test("ReverseAnimation flips value and status", () => {
    const c = new AnimationController({ duration: 100 });
    const r = new ReverseAnimation(c);
    const statuses: AnimationStatus[] = [];
    r.addStatusListener((s) => statuses.push(s));
    expect.equal(r.value, 1);
    expect.equal(r.status, "completed");
    c.forward();
    pumpFor(200);
    expect.equal(r.value, 0);
    expect.deepEqual(statuses, ["reverse", "dismissed"]);
    c.dispose();
  });

  test("Tween / ColorTween / IntTween / CurveTween / chain / drive", () => {
    expect.equal(new Tween(10, 20).transform(0.25), 12.5);
    expect.equal(new IntTween(0, 10).transform(0.26), 3);
    const col = new ColorTween(new Color(0xff000000), new Color(0xffffffff)).transform(0.5);
    expect.equal(col?.value, 0xff808080);
    expect.isNull(new ColorTween(null, null).transform(0.5));
    expect.equal(new CurveTween(Curves.easeIn).transform(1), 1);
    const chained = new Tween(0, 100).chain(new CurveTween(new Interval(0.5, 1)));
    expect.equal(chained.transform(0.25), 0);
    expect.close(chained.transform(0.75), 50, 1e-9);
    const c = new AnimationController({ value: 0.5 });
    expect.equal(c.drive(new Tween(0, 8)).value, 4);
    c.dispose();
  });

  test("Curves endpoints exact and monotonic samples", () => {
    const names = ["linear", "ease", "easeIn", "easeOut", "easeInOut", "fastOutSlowIn", "decelerate",
      "easeInCubic", "easeOutCubic", "easeInOutCubic", "bounceOut", "elasticOut"] as const;
    for (const n of names) {
      const curve = Curves[n];
      expect.equal(curve.transform(0), 0, `${n}(0)`);
      expect.equal(curve.transform(1), 1, `${n}(1)`);
    }
    for (const n of ["linear", "ease", "easeIn", "easeOut", "easeInOut", "fastOutSlowIn", "decelerate"] as const) {
      let prev = 0;
      for (let i = 1; i <= 20; i++) {
        const v = Curves[n].transform(i / 20);
        expect.isTrue(v >= prev - 1e-3, `${n} monotonic at ${i}`);
        prev = v;
      }
    }
    expect.close(Curves.easeIn.flipped.transform(0.3), 1 - Curves.easeIn.transform(0.7), 1e-12);
  });

  test("SpringSimulation: three damping regimes converge", () => {
    const critical = new SpringSimulation(SpringDescription.withDampingRatio({ mass: 1, stiffness: 100, ratio: 1 }), 0, 1, 0);
    const under = new SpringSimulation(new SpringDescription(1, 100, 2), 0, 1, 0);
    const over = new SpringSimulation(new SpringDescription(1, 100, 50), 0, 1, 0);
    expect.equal(critical.type, "criticallyDamped");
    expect.equal(under.type, "underDamped");
    expect.equal(over.type, "overDamped");
    for (const s of [critical, under, over]) {
      expect.close(s.x(0), 0, 1e-9);
      expect.close(s.dx(0), 0, 1e-9);
      expect.close(s.x(20), 1, 1e-3);
      expect.isTrue(s.isDone(20));
      expect.isFalse(s.isDone(0));
    }
    // 欠阻尼会越过终点
    let maxX = 0;
    for (let t = 0; t < 2; t += 0.01) maxX = Math.max(maxX, under.x(t));
    expect.isTrue(maxX > 1.1, "underdamped overshoots");
    // 临界/过阻尼不越过
    for (let t = 0; t < 5; t += 0.01) expect.isTrue(critical.x(t) <= 1 + 1e-9 && over.x(t) <= 1 + 1e-9);
    // 带初速度:dx(0) 等于初速度
    const v = new SpringSimulation(new SpringDescription(1, 100, 2), 0, 1, 5);
    expect.close(v.dx(0), 5, 1e-9);
  });

  test("FrictionSimulation finalX and timeAtX", () => {
    const f = new FrictionSimulation(0.135, 100, 1000);
    const finalX = 100 - 1000 / Math.log(0.135);
    expect.close(f.finalX, finalX, 1e-9);
    expect.close(f.x(50), finalX, 1e-3);
    expect.isTrue(f.isDone(50));
    const mid = (100 + finalX) / 2;
    expect.close(f.x(f.timeAtX(mid)), mid, 1e-6);
    expect.equal(f.timeAtX(finalX + 10), Infinity);
  });

  test("fling settles at upper bound", () => {
    const c = new AnimationController({ duration: 100 });
    const statuses: AnimationStatus[] = [];
    c.addStatusListener((s) => statuses.push(s));
    c.fling({ velocity: 2 });
    pumpFor(3000);
    expect.equal(c.value, 1);
    expect.equal(c.status, "completed");
    expect.isFalse(c.isAnimating);
    c.fling({ velocity: -2 });
    pumpFor(3000);
    expect.equal(c.value, 0);
    expect.deepEqual(statuses, ["forward", "completed", "reverse", "dismissed"]);
    c.dispose();
  });

  test("unbounded controller with animateWith(simulation)", () => {
    const c = AnimationController.unbounded();
    c.animateWith(new FrictionSimulation(0.135, 0, 500));
    pumpFor(10000, 100);
    expect.close(c.value, -500 / Math.log(0.135), 1);
    expect.isFalse(c.isAnimating);
    c.dispose();
  });
});
