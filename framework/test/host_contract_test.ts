// 宿主契约测试: 框架的 Widget / app 与宿主通道(momo/window、momo/app)之间的协议。
// 只用框架的公开接口, 并等待宿主的回复与通知真正到达——所以同一组用例既在 Node 的 mock 宿主下运行,
// 也在真实引擎(momo_headless: Application + WindowManager + 各宿主通道)下运行。mock 宿主是协议的第二份
// 实现, 这些用例把它和真实实现绑在一起: 任何一边改了协议, 这里会失败。
import { group, expect } from "./harness";
import { testViews } from "./util";
import { SizedBox, Widget, app } from "../packages/momo/src/index";

/** 等到 predicate 成立(宿主的消息经真实的事件循环到达)。 */
async function until(predicate: () => boolean, what: string, timeoutMs = 3000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await new Promise<void>((resolve) => setTimeout(resolve, 1));
  }
}

/** 宿主处理完此前发出的调用: 再发一个有回复的调用并等它回来(宿主按序处理)。 */
async function settled(): Promise<void> {
  const probe = await Widget.create({ width: 10, height: 10, show: false });
  probe.destroy();
  await until(() => probe.isDestroyed(), "the probe window to close");
}

group("host contract", () => {
  testViews("the startup window reports its state once, then follows the host", async (t) => {
    await t.pumpView(new SizedBox({}));
    const win = Widget.fromId(0)!;
    await until(() => win.bounds !== null, "the startup window's snapshot");
    expect.equal(win.bounds!.width, 800);
    expect.equal(win.bounds!.height, 600);
    expect.isTrue(win.isVisible);
    expect.isFalse(win.isMinimized);
    expect.isTrue(win.isFocused, "the startup window holds the system focus");
    expect.equal(Widget.getFocused()?.id, 0);
    const moves: number[] = [];
    win.on("move", (b) => moves.push(b.x));
    win.setPosition(30, 40);
    await until(() => win.bounds!.x === 30 && win.bounds!.y === 40, "the window to move");
    expect.deepEqual(moves, [30]);
    win.setPosition(0, 0);
    await until(() => win.bounds!.x === 0, "the window to move back");
  });

  testViews("a created window is ready when create returns; its state changes arrive as events", async (t) => {
    await t.pumpView(new SizedBox({}));
    const first = Widget.fromId(0)!;
    await until(() => first.isFocused, "the startup window's focus");
    const win = await Widget.create({ width: 320, height: 200, x: 50, y: 60, show: false });
    try {
      // create 的回复就是窗口的状态: 不需要再问宿主。
      expect.isTrue(win.id > 0);
      expect.equal(win.viewport?.id, win.id);
      expect.deepEqual(win.bounds, { x: 50, y: 60, width: 320, height: 200 });
      expect.isFalse(win.isVisible, "created hidden");
      expect.isFalse(win.isFocused);
      expect.isTrue(first.isFocused, "a hidden window does not take the focus");

      const events: string[] = [];
      for (const e of ["show", "hide", "minimize", "maximize", "restore", "focus", "blur"] as const) {
        win.on(e, () => events.push(e));
      }
      first.on("blur", () => events.push("first:blur"));
      first.on("focus", () => events.push("first:focus"));

      win.show();
      await until(() => win.isVisible && win.isFocused, "the window to show and take the focus");
      expect.isFalse(first.isFocused, "only one window holds the focus");
      expect.equal(Widget.getFocused()?.id, win.id);
      expect.isTrue(events.includes("show") && events.includes("focus") && events.includes("first:blur"));

      events.length = 0;
      win.minimize();
      await until(() => win.isMinimized, "minimize");
      win.restore();
      await until(() => !win.isMinimized, "restore");
      win.maximize();
      await until(() => win.isMaximized, "maximize");
      win.restore();
      await until(() => !win.isMaximized, "restore from maximized");
      win.hide();
      await until(() => !win.isVisible, "hide");
      expect.deepEqual(events, ["minimize", "restore", "maximize", "restore", "hide"]);

      first.focus();
      await until(() => first.isFocused, "the startup window to take the focus back");
      expect.isFalse(win.isFocused);
    } finally {
      win.destroy();
      await until(() => win.isDestroyed(), "the window to close");
    }
    expect.isNull(Widget.fromId(win.id), "a closed window is gone");
    expect.isNull(win.viewport);
  });

  testViews("close asks the application first; destroy does not", async (t) => {
    await t.pumpView(new SizedBox({}));
    const win = await Widget.create({ width: 200, height: 100 });
    let asked = 0;
    let allow = false;
    let closed = 0;
    win.on("close", (e) => {
      asked++;
      if (!allow) e.preventDefault();
    });
    win.on("closed", () => closed++);
    win.close();
    await until(() => asked === 1, "the close request");
    await settled();
    expect.isFalse(win.isDestroyed(), "the application refused");
    expect.equal(closed, 0);
    allow = true;
    win.close();
    await until(() => win.isDestroyed(), "the window to close");
    expect.equal(asked, 2);
    expect.equal(closed, 1);

    const other = await Widget.create({ width: 200, height: 100 });
    let otherAsked = 0;
    other.on("close", () => otherAsked++);
    other.destroy();
    await until(() => other.isDestroyed(), "destroy");
    expect.equal(otherAsked, 0, "destroy closes without asking");
  });

  testViews("quitting asks before-quit and can be cancelled", async (t) => {
    await t.pumpView(new SizedBox({}));
    let asked = 0;
    const refuse = (e: { preventDefault(): void }): void => {
      asked++;
      e.preventDefault();
    };
    app.on("before-quit", refuse);
    try {
      app.quit();
      await until(() => asked === 1, "before-quit");
      await settled();
      expect.notNull(Widget.fromId(0), "the quit was cancelled: the window is still open");
      expect.isTrue(app.state !== "terminating");
    } finally {
      app.off("before-quit", refuse);
    }
  });
});
