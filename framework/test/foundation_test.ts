import { ChangeNotifier, ValueNotifier, ValueKey, UniqueKey, keysEqual } from "../packages/momo/src/foundation/index";
import { expect, group, test } from "./harness";

group("foundation", () => {
  test("ChangeNotifier: removal during notify is safe", () => {
    const n = new ChangeNotifier();
    const calls: string[] = [];
    const b = (): void => void calls.push("b");
    const a = (): void => {
      calls.push("a");
      n.removeListener(b);
    };
    n.addListener(a);
    n.addListener(b);
    n.notifyListeners();
    expect.deepEqual(calls, ["a"]);
    n.notifyListeners();
    expect.deepEqual(calls, ["a", "a"]);
  });
  test("ValueNotifier notifies only on change", () => {
    const v = new ValueNotifier(1);
    let count = 0;
    v.addListener(() => count++);
    v.value = 1;
    v.value = 2;
    expect.equal(count, 1);
  });
  test("keys", () => {
    expect.isTrue(keysEqual(new ValueKey(1), new ValueKey(1)));
    expect.isFalse(keysEqual(new ValueKey(1), new ValueKey("1")));
    const u = new UniqueKey();
    expect.isTrue(keysEqual(u, u));
    expect.isFalse(keysEqual(u, new UniqueKey()));
    expect.isTrue(keysEqual(null, undefined));
  });
});
