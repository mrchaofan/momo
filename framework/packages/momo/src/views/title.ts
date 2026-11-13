// Title: 声明所在窗口的标题(对标 Flutter Title)。窗口由 WidgetScope 提供(runApp 的启动窗口或 RootView 的窗口);
// 不在任何窗口里时什么也不做。
import type { Key } from "../foundation/key";
import type { Widget } from "../services/widget";
import { State, StatefulView, type BuildContext, type View } from "./framework";
import { WidgetScope } from "./widget_scope";

export class Title extends StatefulView {
  readonly title: string;
  readonly child: View;
  constructor(o: { key?: Key | null; title: string; child: View }) {
    super(o.key);
    this.title = o.title;
    this.child = o.child;
  }
  createState(): State {
    return new TitleState();
  }
}

class TitleState extends State<Title> {
  #applied: { widget: Widget; title: string } | null = null;
  #apply(): void {
    const widget = WidgetScope.maybeOf(this.context);
    const title = this.view.title;
    if (widget === null || (this.#applied?.widget === widget && this.#applied.title === title)) return;
    this.#applied = { widget, title };
    widget.setTitle(title);
  }
  override didChangeDependencies(): void {
    this.#apply();
  }
  override didUpdateView(): void {
    this.#apply();
  }
  build(_context: BuildContext): View {
    return this.view.child;
  }
}
