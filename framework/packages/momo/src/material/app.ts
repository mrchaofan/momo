import type { Key } from "../foundation/key";
import { Title } from "../views/title";
import { StatelessView, type BuildContext, type View } from "../views/framework";
import { Theme, ThemeData } from "./theme";

/** Material 应用根: 主题, 以及所在窗口的标题(runApp 时为启动窗口;不在任何窗口里时只提供主题)。 */
export class MaterialApp extends StatelessView {
  readonly title: string | null;
  readonly theme: ThemeData;
  readonly home: View;
  constructor(o: { key?: Key | null; title?: string | null; theme?: ThemeData; home: View }) {
    super(o.key);
    this.title = o.title ?? null;
    this.theme = o.theme ?? new ThemeData();
    this.home = o.home;
  }
  build(_context: BuildContext): View {
    const content = new Theme({ data: this.theme, child: this.home });
    return this.title !== null ? new Title({ title: this.title, child: content }) : content;
  }
}
