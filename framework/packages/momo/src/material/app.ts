import { window } from "momo:ui";
import type { Key } from "../foundation/key";
import { State, StatefulWidget, type BuildContext, type Widget } from "../widgets/framework";
import { Theme, ThemeData } from "./theme";

/** Material 应用根:设置窗口标题与主题。 */
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
  #applyTitle(): void {
    if (this.widget.title !== null) window.title = this.widget.title;
  }
  override initState(): void {
    this.#applyTitle();
  }
  override didUpdateWidget(old: MaterialApp): void {
    if (old.title !== this.widget.title) this.#applyTitle();
  }
  build(_context: BuildContext): Widget {
    return new Theme({ data: this.widget.theme, child: this.widget.home });
  }
}
