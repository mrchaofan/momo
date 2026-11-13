import {
  type BuildContext, Column, GlobalKey, InheritedView, Key, Padding, EdgeInsets, Row, SizedBox, State,
  StatefulView, StatelessView, Text, ValueKey, View, Center, Element, StatefulElement, Builder,
} from "../packages/momo/src/index";
import { expect, group } from "./harness";
import { testViews } from "./util";

/** 记录构建次数与 State 身份的组件。 */
let nextStateId = 1;
const builds: string[] = [];

class Probe extends StatefulView {
  constructor(readonly label: string, key?: Key | null) {
    super(key);
  }
  createState(): State {
    return new ProbeState();
  }
}
class ProbeState extends State<Probe> {
  readonly id = nextStateId++;
  disposed = false;
  updated = 0;
  override didUpdateView(): void {
    this.updated++;
  }
  override dispose(): void {
    this.disposed = true;
  }
  build(): View {
    builds.push(this.view.label);
    return new SizedBox({ width: 10, height: 10 });
  }
}

function probeStates(els: Element[]): ProbeState[] {
  return els.filter((e): e is StatefulElement => e instanceof StatefulElement && e.state instanceof ProbeState).map((e) => e.state as ProbeState);
}

class Holder extends StatefulView {
  constructor(readonly build0: (s: HolderState) => View, key?: Key | null) {
    super(key);
  }
  createState(): State {
    return new HolderState();
  }
}
class HolderState extends State<Holder> {
  value = 0;
  bump(): void {
    this.setState(() => this.value++);
  }
  build(): View {
    return this.view.build0(this);
  }
}

class Data extends InheritedView {
  constructor(readonly value: number, child: View) {
    super({ child });
  }
  updateShouldNotify(old: Data): boolean {
    return old.value !== this.value;
  }
  static of(c: BuildContext): number {
    return c.dependOnInheritedViewOfExactType(Data)?.value ?? -1;
  }
}

class Consumer extends StatelessView {
  constructor(readonly name: string) {
    super();
  }
  build(c: BuildContext): View {
    builds.push(`${this.name}:${Data.of(c)}`);
    return new SizedBox();
  }
}

class NonConsumer extends StatelessView {
  build(): View {
    builds.push("non");
    return new SizedBox();
  }
}

const constNonConsumer = new NonConsumer();
const constConsumer = new Consumer("c");

group("views/framework", () => {
  testViews("setState rebuilds only the dirty subtree", async (t) => {
    builds.length = 0;
    const key = new GlobalKey<HolderState>();
    await t.pumpView(new Column({
      children: [
        new Holder((s) => new Probe(`a${s.value}`), key),
        new Probe("b"),
      ],
    }));
    expect.deepEqual(builds, ["a0", "b"]);
    builds.length = 0;
    key.currentState!.bump();
    await t.pump();
    expect.deepEqual(builds, ["a1"]);
    // 状态被保留(didUpdateView 而非重建 State)
    const states = probeStates(t.allElements());
    expect.equal(states.length, 2);
    expect.equal(states[0]!.updated, 1);
  });

  testViews("keyed children keep State across reorder", async (t) => {
    const key = new GlobalKey<HolderState>();
    const order = (s: HolderState): string[] => (s.value % 2 === 0 ? ["x", "y", "z"] : ["z", "x", "y"]);
    await t.pumpView(new Holder((s) => new Column({ children: order(s).map((l) => new Probe(l, new ValueKey(l))) }), key));
    const before = new Map(probeStates(t.allElements()).map((s) => [s.view.label, s.id]));
    key.currentState!.bump();
    await t.pump();
    const after = probeStates(t.allElements());
    expect.deepEqual(after.map((s) => s.view.label), ["z", "x", "y"]);
    for (const s of after) expect.equal(s.id, before.get(s.view.label), `state for ${s.view.label} reused`);
    // 渲染顺序同步移动
    const ys = after.map((s) => t.rectOf(s.context).top);
    expect.deepEqual(ys, [0, 10, 20].map((v) => v));
  });

  testViews("unkeyed children are matched by position; removal disposes", async (t) => {
    const key = new GlobalKey<HolderState>();
    await t.pumpView(new Holder((s) => new Column({ children: (s.value === 0 ? ["p", "q", "r"] : ["p", "r"]).map((l) => new Probe(l)) }), key));
    const [p, q, r] = probeStates(t.allElements());
    key.currentState!.bump();
    await t.pump();
    const after = probeStates(t.allElements());
    expect.equal(after.length, 2);
    // 位置匹配:第二个 State(q 的)复用给 "r", 原 r 的 State 被释放
    expect.equal(after[0]!.id, p!.id);
    expect.equal(after[1]!.id, q!.id);
    expect.equal(after[1]!.view.label, "r");
    expect.isTrue(r!.disposed);
  });

  testViews("canUpdate: different key forces a new State", async (t) => {
    const key = new GlobalKey<HolderState>();
    await t.pumpView(new Holder((s) => new Probe("k", new ValueKey(s.value)), key));
    const first = probeStates(t.allElements())[0]!;
    key.currentState!.bump();
    await t.pump();
    const second = probeStates(t.allElements())[0]!;
    expect.notEqual(second.id, first.id);
    expect.isTrue(first.disposed);
  });

  testViews("GlobalKey reparenting preserves State", async (t) => {
    const holder = new GlobalKey<HolderState>();
    const probeKey = new GlobalKey<ProbeState>();
    await t.pumpView(new Holder((s) => s.value === 0
      ? new Column({ children: [new Padding({ padding: EdgeInsets.all(5), child: new Probe("g", probeKey) })] })
      : new Row({ children: [new SizedBox({ width: 3 }), new Center({ child: new Probe("g", probeKey) })] }), holder));
    const st = probeKey.currentState!;
    holder.currentState!.bump();
    await t.pump();
    expect.equal(probeKey.currentState, st);
    expect.isFalse(st.disposed);
    expect.equal(t.rectOf(probeKey.currentContext!).left, 3);
  });

  testViews("InheritedView rebuilds dependents only", async (t) => {
    builds.length = 0;
    const key = new GlobalKey<HolderState>();
    await t.pumpView(new Holder((s) => new Data(s.value < 2 ? 0 : s.value, new Column({ children: [constConsumer, constNonConsumer] })), key));
    expect.deepEqual(builds, ["c:0", "non"]);
    builds.length = 0;
    key.currentState!.bump(); // value 1 → Data 仍为 0, 不通知
    await t.pump();
    expect.deepEqual(builds, []);
    key.currentState!.bump(); // value 2 → Data 2
    await t.pump();
    expect.deepEqual(builds, ["c:2"]);
  });

  testViews("build errors are reported and replaced by ErrorView", async (t) => {
    const errors: unknown[] = [];
    const { setErrorReporter } = await import("../packages/momo/src/foundation/assert");
    const old = setErrorReporter((e) => errors.push(e));
    try {
      await t.pumpView(new Builder({ builder: () => { throw new Error("boom"); } }));
      expect.equal(errors.length, 1);
      expect.equal(t.byType(Text).length, 0);
    } finally {
      setErrorReporter(old);
    }
  });
});
