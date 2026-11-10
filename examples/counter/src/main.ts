// examples/counter — Widget 版计数器(docs/11 M4 验收):文字 + 按钮 + 点击计数。
// 构建: node framework/scripts/build_example.mjs examples/counter/src/main.ts examples/counter/dist/main.js
import {
  AppBar, Center, Column, EdgeInsets, ElevatedButton, FloatingActionButton, Intent, CallbackAction, LogicalKeyboardKey,
  MaterialApp, Padding, Row, Scaffold, SingleActivator, SizedBox, State, StatefulWidget, Shortcuts, Actions, Text, TextButton,
  Theme, ThemeData, Color, Focus, runApp, type BuildContext, type Widget,
} from "@momo/framework";

class IncrementIntent extends Intent {}
class DecrementIntent extends Intent {}

class CounterPage extends StatefulWidget {
  constructor(readonly title: string) {
    super();
  }
  createState(): State {
    return new CounterPageState();
  }
}

class CounterPageState extends State<CounterPage> {
  #count = 0;

  #increment = (): void => this.setState(() => this.#count++);
  #decrement = (): void => this.setState(() => this.#count--);
  #reset = (): void => this.setState(() => (this.#count = 0));

  build(context: BuildContext): Widget {
    const theme = Theme.of(context);
    return new Shortcuts({
      // 键盘:↑ / + 加一, ↓ / - 减一
      shortcuts: new Map([
        [new SingleActivator(LogicalKeyboardKey.arrowUp), new IncrementIntent()],
        [new SingleActivator(LogicalKeyboardKey.equal), new IncrementIntent()],
        [new SingleActivator(LogicalKeyboardKey.arrowDown), new DecrementIntent()],
        [new SingleActivator(LogicalKeyboardKey.minus), new DecrementIntent()],
      ]),
      child: new Actions({
        actions: new Map([
          [IncrementIntent, new CallbackAction<IncrementIntent>({ onInvoke: this.#increment })],
          [DecrementIntent, new CallbackAction<DecrementIntent>({ onInvoke: this.#decrement })],
        ]),
        child: new Focus({
          autofocus: true,
          child: new Scaffold({
            appBar: new AppBar({ title: new Text(this.widget.title) }),
            body: new Center({
              child: new Column({
                mainAxisSize: "min",
                children: [
                  new Text("You have pushed the button this many times:"),
                  new SizedBox({ height: 8 }),
                  new Text(`${this.#count}`, { style: theme.textTheme.headlineMedium.copyWith({ fontSize: 48, color: theme.colorScheme.primary }) }),
                  new SizedBox({ height: 24 }),
                  new Row({
                    mainAxisSize: "min",
                    children: [
                      new ElevatedButton({ onPressed: this.#decrement, child: new Text("−  Decrement") }),
                      new SizedBox({ width: 12 }),
                      new ElevatedButton({ onPressed: this.#increment, child: new Text("+  Increment") }),
                    ],
                  }),
                  new Padding({
                    padding: EdgeInsets.only({ top: 12 }),
                    child: new TextButton({ onPressed: this.#count !== 0 ? this.#reset : null, child: new Text("Reset") }),
                  }),
                ],
              }),
            }),
            floatingActionButton: new FloatingActionButton({ onPressed: this.#increment, child: new Text("+") }),
          }),
        }),
      }),
    });
  }
}

runApp(new MaterialApp({
  title: "Momo · counter",
  theme: new ThemeData({ primaryColor: new Color(0xff4285f4) }),
  home: new CounterPage("Momo Counter"),
}));
