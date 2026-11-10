import {
  AlertDialog, AppBar, Builder, Center, Checkbox, ElevatedButton, FloatingActionButton, InputDecoration, LogicalKeyboardKey,
  MaterialApp, Offset, PhysicalKeyboardKey, Scaffold, State, StatefulWidget, Text, TextButton, TextField, ThemeData, showDialog,
  closeDialog, FocusNode, ButtonState, StatefulElement, type BuildContext, type Widget, Container, TextEditingController,
} from "../packages/momo/src/index";
import { expect, group } from "./harness";
import { testWidgets } from "./util";

const app = (home: Widget): Widget => new MaterialApp({ title: "test", theme: new ThemeData(), home });

class CheckHost extends StatefulWidget {
  createState(): State {
    return new CheckHostState();
  }
}
class CheckHostState extends State<CheckHost> {
  value = false;
  build(): Widget {
    return new Center({ child: new Checkbox({ value: this.value, onChanged: (v) => this.setState(() => (this.value = v)) }) });
  }
}

function buttonState(t: { allElements(): import("../packages/momo/src/index").Element[] }): ButtonState {
  const e = t.allElements().find((x) => x instanceof StatefulElement && x.state instanceof ButtonState) as StatefulElement;
  return e.state as ButtonState;
}

group("material", () => {
  testWidgets("MaterialApp sets the window title", async (_t, ctx) => {
    await _t.pumpWidget(app(new Text("x")));
    expect.equal(ctx.host.mock!.title, "test");
  }, { mockOnly: true });

  testWidgets("ElevatedButton: hover / pressed states, onPressed, disabled", async (t) => {
    let presses = 0;
    await t.pumpWidget(app(new Center({ child: new ElevatedButton({ onPressed: () => presses++, child: new Text("Go") }) })));
    const btn = t.one(t.byType(ElevatedButton));
    const s = buttonState(t);
    await t.hover(t.centerOf(btn));
    expect.isTrue(s.hovered);
    expect.equal(t.binding.mouseTracker.currentCursor, "click");
    t.sendPointer(1, t.centerOf(btn));
    await t.pumpFor(150);
    expect.isTrue(s.pressed, "pressed after tapDown deadline");
    t.sendPointer(3, t.centerOf(btn), { buttons: 0 });
    await t.pump();
    expect.isFalse(s.pressed);
    expect.equal(presses, 1);
    await t.hover(new Offset(1, 1));
    expect.isFalse(s.hovered);
    await t.pumpWidget(app(new Center({ child: new ElevatedButton({ onPressed: null, child: new Text("Go") }) })));
    await t.tap(t.one(t.byType(ElevatedButton)));
    expect.equal(presses, 1);
  });

  testWidgets("Checkbox toggles by tap and by Space when focused", async (t) => {
    await t.pumpWidget(app(new CheckHost()));
    const host = t.allElements().find((e) => e instanceof StatefulElement && e.state instanceof CheckHostState) as StatefulElement;
    const st = host.state as CheckHostState;
    await t.tap(t.one(t.byType(Checkbox)));
    expect.isTrue(st.value);
    await t.pressKey({ logical: LogicalKeyboardKey.tab.keyId, physical: PhysicalKeyboardKey.tab.keyId });
    await t.pressKey({ logical: LogicalKeyboardKey.space.keyId, physical: PhysicalKeyboardKey.space.keyId, character: " " });
    expect.isFalse(st.value);
  });

  testWidgets("TextField: hint, typing via IME commit, onChanged", async (t) => {
    t.recordMethodCalls("momo/textinput");
    const changes: string[] = [];
    const node = new FocusNode();
    const controller = new TextEditingController();
    await t.pumpWidget(app(new Center({ child: new Container({ width: 300, child: new TextField({ controller, focusNode: node, decoration: new InputDecoration({ hintText: "Type here" }), onChanged: (s) => changes.push(s) }) }) })));
    expect.equal(t.byText("Type here").length, 1);
    await t.tap(t.one(t.byType(TextField)));
    expect.isTrue(node.hasFocus);
    await t.platformCall("momo/textinput", "commit", { text: "hi" });
    await t.pump();
    expect.deepEqual(changes, ["hi"]);
    expect.equal(controller.text, "hi");
    expect.equal(t.byText("Type here").length, 0);
  });

  testWidgets("Scaffold: app bar and FAB placement", async (t) => {
    await t.pumpWidget(app(new Scaffold({
      appBar: new AppBar({ title: new Text("Title") }),
      body: new Center({ child: new Text("body") }),
      floatingActionButton: new FloatingActionButton({ onPressed: () => {}, child: new Text("+") }),
    })));
    const bar = t.rectOf(t.one(t.byType(AppBar)));
    expect.deepEqual([bar.top, bar.height, bar.width], [0, 56, 800]);
    const fab = t.rectOf(t.one(t.byType(FloatingActionButton)));
    expect.deepEqual([fab.right, fab.bottom], [784, 584]);
  });

  testWidgets("showDialog: open, close with result, barrier dismiss, Escape", async (t) => {
    let ctxRef: BuildContext | null = null;
    await t.pumpWidget(app(new Builder({ builder: (c) => {
      ctxRef = c;
      return new Center({ child: new Text("home") });
    } })));
    const open = (): Promise<unknown> => showDialog<string>({
      context: ctxRef!,
      builder: (_c, close) => new AlertDialog({
        title: new Text("Delete?"),
        content: new Text("This cannot be undone."),
        actions: [
          new TextButton({ onPressed: () => close("no"), child: new Text("Cancel") }),
          new Builder({ builder: (c) => new TextButton({ onPressed: () => closeDialog(c, "yes"), child: new Text("OK") }) }),
        ],
      }),
    });
    let p = open();
    await t.pump();
    expect.equal(t.byText("Delete?").length, 1);
    await t.tap(t.one(t.byText("OK")));
    expect.equal(await p, "yes");
    await t.pump();
    expect.equal(t.byText("Delete?").length, 0);
    p = open();
    await t.pump();
    await t.tap(new Offset(5, 5));
    expect.equal(await p, undefined);
    p = open();
    await t.pump();
    await t.pressKey({ logical: LogicalKeyboardKey.escape.keyId, physical: PhysicalKeyboardKey.escape.keyId });
    expect.equal(await p, undefined);
    await t.pump();
    expect.equal(t.byText("Delete?").length, 0);
  });
});
