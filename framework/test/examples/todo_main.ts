// 在 mock 宿主下运行真实的 examples/todo bundle 并驱动交互(端到端冒烟)。
import { __mock } from "../mock/momo_ui";
import "../../../examples/todo/src/main";
import { runRegisteredTests, test, expect } from "../harness";
import { ViewTester } from "../tester";
import {
  Checkbox, LogicalKeyboardKey, PhysicalKeyboardKey, Size, Offset, ScrollableState, StatefulElement,
} from "../../packages/momo/src/index";

test("examples/todo: type, add, check, delete, scroll", async () => {
  const t = new ViewTester(new Size(800, 600));
  const calls = t.recordMethodCalls("momo/textinput");
  await t.pumpFor(300);
  expect.equal(__mock.title, "Momo · todo");
  expect.equal(t.byText("3 项未完成 · 共 3 项").length, 0, "one item starts done");
  expect.equal(t.byText("2 项未完成 · 共 3 项").length, 1);
  // 输入框自动获得焦点并打开输入法
  expect.isTrue(calls.some((c) => c.method === "show"));
  await t.platformCall("momo/textinput", "composing", { text: "mai", cursor: 3 });
  await t.platformCall("momo/textinput", "commit", { text: "买牛奶" });
  await t.pressKey({ logical: LogicalKeyboardKey.enter.keyId, physical: PhysicalKeyboardKey.enter.keyId });
  await t.pumpFor(300);
  expect.equal(t.byText("买牛奶").length, 1);
  expect.equal(t.byText("3 项未完成 · 共 4 项").length, 1);
  // 点击"添加"按钮添加
  await t.platformCall("momo/textinput", "commit", { text: "写测试" });
  await t.tap(t.one(t.byText("添加")));
  await t.pumpFor(300);
  expect.equal(t.byText("4 项未完成 · 共 5 项").length, 1);
  // 勾选第一个未完成项
  const boxes = t.byType(Checkbox);
  await t.tap(boxes[1]!);
  await t.pumpFor(300);
  expect.equal(t.byText("3 项未完成 · 共 5 项").length, 1);
  // 删除第一项
  await t.tap(t.byText("删除")[0]!);
  await t.pumpFor(300);
  expect.equal(t.byText("3 项未完成 · 共 4 项").length, 1);
  // 清除已完成
  await t.tap(t.one(t.byText("清除已完成")));
  await t.pumpFor(300);
  expect.equal(t.byText("3 项未完成 · 共 3 项").length, 1);
  // 添加很多项后滚动列表
  for (let i = 0; i < 30; i++) {
    await t.platformCall("momo/textinput", "commit", { text: `任务 ${i}` });
    await t.pressKey({ logical: LogicalKeyboardKey.enter.keyId, physical: PhysicalKeyboardKey.enter.keyId });
  }
  await t.pumpFor(300);
  const scrollable = t.allElements().find((e) => e instanceof StatefulElement && e.state instanceof ScrollableState) as StatefulElement;
  const pos = (scrollable.state as ScrollableState).position;
  expect.isTrue(pos.maxScrollExtent > 0, `list overflows: ${pos.maxScrollExtent}`);
  await t.scroll(new Offset(400, 300), 400);
  expect.equal(pos.pixels, Math.min(400, pos.maxScrollExtent));
  expect.equal(t.byText("任务 29").length === 1 || pos.maxScrollExtent > 400, true);
});

const proc = (globalThis as unknown as { process: { exitCode?: number } }).process;
const s = await runRegisteredTests({ name: "node-mock (todo example)", log: (l) => console.log(l), ahemText: true, mock: __mock });
if (s.failed > 0) proc.exitCode = 1;
