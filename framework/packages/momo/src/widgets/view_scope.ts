// 向子树提供"所在的窗口"(momo:ui View):主窗口为 window, 其他窗口由 View widget 提供。
import type { View as UiView } from "momo:ui";
import type { Key } from "../foundation/key";
import { InheritedWidget, type BuildContext, type Widget } from "./framework";

export class ViewScope extends InheritedWidget {
  readonly view: UiView;
  constructor(o: { key?: Key | null; view: UiView; child: Widget }) {
    super(o);
    this.view = o.view;
  }
  /** 子树所在的窗口;不在任何窗口中时为 null。 */
  static maybeOf(context: BuildContext): UiView | null {
    return context.dependOnInheritedWidgetOfExactType(ViewScope)?.view ?? null;
  }
  updateShouldNotify(old: ViewScope): boolean {
    return old.view !== this.view;
  }
}
