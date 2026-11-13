import {
  Center, Container, Colors, GestureDetector, State, StatefulView, Text, type View, type BuildContext,

} from "../packages/momo/src/index";
import { expect, group } from "./harness";
import { testViews } from "./util";

class Counter extends StatefulView {
  createState(): State { return new CounterState(); }
}
class CounterState extends State<Counter> {
  count = 0;
  build(_c: BuildContext): View {
    return new Center({
      child: new GestureDetector({
        onTap: () => this.setState(() => this.count++),
        child: new Container({ width: 100, height: 40, color: Colors.blue, child: new Text(`count ${this.count}`) }),
      }),
    });
  }
}

group("smoke", () => {
  testViews("counter tap", async (t) => {
    await t.pumpView(new Counter());
    expect.equal(t.byText("count 0").length, 1);
    const box = t.one(t.byType(Container));
    expect.deepEqual(t.rectOf(box).toString(), "Rect.fromLTRB(350.0, 280.0, 450.0, 320.0)");
    await t.tap(box);
    await t.pump();
    expect.equal(t.byText("count 1").length, 1);
  });
});
