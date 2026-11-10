// 在 mock 宿主下运行真实的 examples/counter bundle 并驱动交互(端到端冒烟)。
import { __mock } from "../mock/momo_ui";
import "../../../examples/counter/src/main";
import { runRegisteredTests, test, expect } from "../harness";
import { WidgetTester } from "../tester";
import { LogicalKeyboardKey, PhysicalKeyboardKey, Size } from "../../packages/momo/src/index";

test("examples/counter: tap buttons, keyboard shortcut, FAB", async () => {
  const t = new WidgetTester(new Size(800, 600));
  await t.pumpAndSettle();
  expect.equal(__mock.title, "Momo · counter");
  expect.equal(t.byText("0").length, 1);
  await t.tap(t.one(t.byText("+  Increment")));
  await t.pumpAndSettle();
  expect.equal(t.byText("1").length, 1);
  await t.tap(t.one(t.byText("−  Decrement")));
  await t.tap(t.one(t.byText("−  Decrement")));
  await t.pumpAndSettle();
  expect.equal(t.byText("-1").length, 1);
  await t.pressKey({ logical: LogicalKeyboardKey.arrowUp.keyId, physical: PhysicalKeyboardKey.arrowUp.keyId });
  await t.pressKey({ logical: LogicalKeyboardKey.arrowUp.keyId, physical: PhysicalKeyboardKey.arrowUp.keyId });
  expect.equal(t.byText("1").length, 1);
  await t.tap(t.one(t.byText("+")));
  await t.pumpAndSettle();
  expect.equal(t.byText("2").length, 1);
  expect.isTrue(__mock.renderedScenes > 0);
});

const proc = (globalThis as unknown as { process: { exitCode?: number } }).process;
const s = await runRegisteredTests({ name: "node-mock (counter example)", log: (l) => console.log(l), ahemText: true, mock: __mock });
if (s.failed > 0) proc.exitCode = 1;
