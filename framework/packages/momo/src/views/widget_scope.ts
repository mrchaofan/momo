// 向子树提供"所在的窗口"(Widget): 隐式视图 0 的窗口由 runApp 的根提供, 其他窗口由 RootView 提供。
import type { Key } from "../foundation/key";
import type { Widget } from "../services/widget";
import { InheritedView, type BuildContext, type View } from "./framework";

export class WidgetScope extends InheritedView {
  readonly widget: Widget;
  constructor(o: { key?: Key | null; widget: Widget; child: View }) {
    super(o);
    this.widget = o.widget;
  }
  /** 子树所在的窗口;不在任何窗口中时为 null。 */
  static maybeOf(context: BuildContext): Widget | null {
    return context.dependOnInheritedViewOfExactType(WidgetScope)?.widget ?? null;
  }
  /** 子树所在的窗口(不在任何窗口中时抛错)。 */
  static of(context: BuildContext): Widget {
    const widget = WidgetScope.maybeOf(context);
    if (widget === null) throw new Error("WidgetScope.of: the context is not inside a window");
    return widget;
  }
  updateShouldNotify(old: WidgetScope): boolean {
    return old.widget !== this.widget;
  }
}
