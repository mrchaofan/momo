import type { Key } from "../foundation/key";
import { WindowService } from "../services/window";
import { ViewScope } from "../widgets/view_scope";
import { State, StatefulWidget, type BuildContext, type Widget } from "../widgets/framework";
import { Theme, ThemeData } from "./theme";

/** Material 应用根: 主题, 以及所在窗口的标题(runApp 时为隐式视图;不在任何窗口里时只提供主题)。 */
export class MaterialApp extends StatefulWidget {
  readonly title: string | null;
  readonly theme: ThemeData;
  readonly home: Widget;
  constructor(o: { key?: Key | null; title?: string | null; theme?: ThemeData; home: Widget }) {
    super(o.key);
    this.title = o.title ?? null;
    this.theme = o.theme ?? new ThemeData();
    this.home = o.home;
  }
  createState(): State {
    return new MaterialAppState();
  }
}

class MaterialAppState extends State<MaterialApp> {
  #applied: string | null = null;
  #applyTitle(): void {
    const view = ViewScope.maybeOf(this.context);
    const title = this.widget.title;
    if (view === null || title === null) return;
    const key = `${view.id}:${title}`;
    if (key === this.#applied) return;
    this.#applied = key;
    WindowService.instance.setTitle(view.id, title);
  }
  override didChangeDependencies(): void {
    this.#applyTitle();
  }
  override didUpdateWidget(): void {
    this.#applyTitle();
  }
  build(_context: BuildContext): Widget {
    return new Theme({ data: this.widget.theme, child: this.widget.home });
  }
}
