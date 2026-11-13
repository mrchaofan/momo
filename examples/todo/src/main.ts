// examples/todo — View 版待办(docs/11 M4 验收):TextField 输入、添加、勾选、删除、列表滚动。
// 构建: node framework/scripts/build_example.mjs examples/todo/src/main.ts examples/todo/dist/main.js
import {
  AppBar, Checkbox, Color, Colors, Column, Container, Divider, EdgeInsets, ElevatedButton, Expanded, InputDecoration, ListTile,
  ListView, MaterialApp, Padding, Row, Scaffold, SizedBox, State, StatefulView, Text, TextButton, TextEditingController, TextField,
  TextStyle, Theme, ThemeData, ValueKey, FocusNode, Center, app, runApp, type BuildContext, type View,
} from "@momo/framework";

/** 待办项。 */
interface TodoItem {
  readonly id: number;
  readonly title: string;
  readonly done: boolean;
}

class TodoApp extends StatefulView {
  createState(): State {
    return new TodoAppState();
  }
}

class TodoAppState extends State<TodoApp> {
  #items: TodoItem[] = [
    { id: 1, title: "读 docs/06-framework-js.md", done: true },
    { id: 2, title: "用 View 写一个 todo", done: false },
    { id: 3, title: "试试滚轮滚动列表", done: false },
  ];
  #nextId = 4;
  readonly #controller = new TextEditingController();
  readonly #inputFocus = new FocusNode({ debugLabel: "todo input" });

  override dispose(): void {
    this.#controller.dispose();
    this.#inputFocus.dispose();
  }

  #add = (): void => {
    const title = this.#controller.text.trim();
    if (title.length === 0) return;
    this.setState(() => {
      this.#items = [...this.#items, { id: this.#nextId++, title, done: false }];
    });
    this.#controller.clear();
    this.#inputFocus.requestFocus();
  };

  #toggle(id: number, done: boolean): void {
    this.setState(() => {
      this.#items = this.#items.map((t) => (t.id === id ? { ...t, done } : t));
    });
  }

  #remove(id: number): void {
    this.setState(() => {
      this.#items = this.#items.filter((t) => t.id !== id);
    });
  }

  #clearDone = (): void => {
    this.setState(() => {
      this.#items = this.#items.filter((t) => !t.done);
    });
  };

  #buildItem(context: BuildContext, item: TodoItem): View {
    const theme = Theme.of(context);
    return new ListTile({
      key: new ValueKey(item.id),
      leading: new Checkbox({ value: item.done, onChanged: (v) => this.#toggle(item.id, v) }),
      title: new Text(item.title, {
        style: item.done
          ? new TextStyle({ decoration: "lineThrough", color: theme.colorScheme.onSurface.withOpacity(0.45) })
          : null,
        maxLines: 1,
        overflow: "ellipsis",
      }),
      trailing: new TextButton({ onPressed: () => this.#remove(item.id), child: new Text("删除") }),
      onTap: () => this.#toggle(item.id, !item.done),
    });
  }

  build(context: BuildContext): View {
    const theme = Theme.of(context);
    const remaining = this.#items.filter((t) => !t.done).length;
    const input = new Padding({
      padding: EdgeInsets.all(16),
      child: new Row({
        children: [
          new Expanded({
            child: new TextField({
              controller: this.#controller,
              focusNode: this.#inputFocus,
              autofocus: true,
              decoration: new InputDecoration({ hintText: "要做点什么?回车添加" }),
              onSubmitted: () => this.#add(),
            }),
          }),
          new SizedBox({ width: 12 }),
          new ElevatedButton({ onPressed: this.#add, child: new Text("添加") }),
        ],
      }),
    });
    const list = this.#items.length === 0
      ? new Center({ child: new Text("没有待办事项 🎉", { style: theme.textTheme.bodyLarge.copyWith({ color: Colors.grey }) }) })
      : ListView.separated({
          itemCount: this.#items.length,
          itemBuilder: (ctx, i) => this.#buildItem(ctx, this.#items[i]!),
          separatorBuilder: () => new Divider({ height: 1, indent: 16 }),
        });
    const footer = new Container({
      padding: EdgeInsets.symmetric({ horizontal: 16, vertical: 8 }),
      color: theme.colorScheme.surface,
      child: new Row({
        children: [
          new Expanded({ child: new Text(`${remaining} 项未完成 · 共 ${this.#items.length} 项`) }),
          new TextButton({ onPressed: this.#items.some((t) => t.done) ? this.#clearDone : null, child: new Text("清除已完成") }),
        ],
      }),
    });
    return new Scaffold({
      appBar: new AppBar({ title: new Text("Momo Todo") }),
      body: new Column({
        crossAxisAlignment: "stretch",
        children: [input, new Divider({ height: 1 }), new Expanded({ child: list }), new Divider({ height: 1 }), footer],
      }),
    });
  }
}

// 单窗口应用: 关窗即退出(应用生命周期与窗口无关, 最后一个窗口关闭后由应用决定是否退出)。
app.on("window-all-closed", () => app.quit());

runApp(new MaterialApp({
  title: "Momo · todo",
  theme: new ThemeData({ primaryColor: new Color(0xff00897b) }),
  home: new TodoApp(),
}));
