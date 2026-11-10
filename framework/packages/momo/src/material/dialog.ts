import type { Key } from "../foundation/key";
import { BorderRadius } from "../painting/borders";
import { Color } from "../painting/color";
import { BoxDecoration, BoxShadow } from "../painting/decoration";
import { EdgeInsets } from "../painting/edge_insets";
import { Offset } from "../painting/geometry";
import { BoxConstraints } from "../rendering/box_constraints";
import { Actions, CallbackAction, DismissIntent } from "../widgets/actions";
import { Center, Column, ConstrainedBox, Padding, Row, SizedBox, Stack, Positioned } from "../widgets/basic";
import { Container } from "../widgets/container";
import { FocusManager, type FocusNode } from "../widgets/focus_manager";
import { FocusScope } from "../widgets/focus_scope";
import { InheritedWidget, StatelessWidget, type BuildContext, type Widget } from "../widgets/framework";
import { GestureDetector } from "../widgets/gesture_detector";
import { Overlay, OverlayEntry } from "../widgets/overlay";
import { DefaultTextStyle } from "../widgets/text";
import { Theme } from "./theme";

class DialogScope extends InheritedWidget {
  constructor(readonly close: (result?: unknown) => void, child: Widget) {
    super({ child });
  }
  updateShouldNotify(old: DialogScope): boolean {
    return old.close !== this.close;
  }
}

/** 关闭最近的对话框(对应 Navigator.pop)。返回是否找到对话框。 */
export function closeDialog(context: BuildContext, result?: unknown): boolean {
  const scope = context.getInheritedWidgetOfExactType(DialogScope);
  if (scope === null) return false;
  scope.close(result);
  return true;
}

/**
 * 显示模态对话框:在最近的 Overlay 顶部插入遮罩 + 居中内容;返回关闭时的结果。
 * builder 的第二个参数(或 {@link closeDialog})用于关闭。
 */
export function showDialog<T = unknown>(o: {
  context: BuildContext;
  builder: (context: BuildContext, close: (result?: T) => void) => Widget;
  barrierDismissible?: boolean;
  barrierColor?: Color;
}): Promise<T | undefined> {
  const overlay = Overlay.of(o.context);
  const previousFocus: FocusNode | null = FocusManager.instance.primaryFocus;
  let entry: OverlayEntry | null = null;
  let resolve: ((v: T | undefined) => void) | null = null;
  const promise = new Promise<T | undefined>((r) => (resolve = r));
  const close = (result?: unknown): void => {
    if (entry === null) return;
    entry.remove();
    entry = null;
    if (previousFocus !== null && previousFocus.context?.mounted) previousFocus.requestFocus();
    resolve?.(result as T | undefined);
  };
  const dismissible = o.barrierDismissible ?? true;
  entry = new OverlayEntry({
    builder: (context) => new DialogScope(close, new Actions({
      actions: new Map([[DismissIntent, new CallbackAction<DismissIntent>({ onInvoke: () => (dismissible ? close(undefined) : null) })]]),
      child: new FocusScope({
        autofocus: true,
        debugLabel: "Dialog Scope",
        child: new Stack({
          fit: "expand",
          children: [
            new Positioned({
              left: 0, top: 0, right: 0, bottom: 0,
              child: new GestureDetector({
                behavior: "opaque",
                onTap: dismissible ? () => close(undefined) : () => {},
                child: new Container({ color: o.barrierColor ?? new Color(0x8a000000) }),
              }),
            }),
            new Center({ child: o.builder(context, close as (r?: T) => void) }),
          ],
        }),
      }),
    })),
  });
  overlay.insert(entry);
  return promise;
}

/** 对话框外壳。 */
export class Dialog extends StatelessWidget {
  readonly child: Widget | null;
  readonly backgroundColor: Color | null;
  constructor(o: { key?: Key | null; child?: Widget | null; backgroundColor?: Color | null } = {}) {
    super(o.key);
    this.child = o.child ?? null;
    this.backgroundColor = o.backgroundColor ?? null;
  }
  build(context: BuildContext): Widget {
    const theme = Theme.of(context);
    return new ConstrainedBox({
      constraints: BoxConstraints.from({ minWidth: 280, maxWidth: 560 }),
      child: new Container({
        decoration: new BoxDecoration({
          color: this.backgroundColor ?? theme.colorScheme.surface,
          borderRadius: BorderRadius.circular(28),
          boxShadow: [new BoxShadow({ color: new Color(0x40000000), offset: new Offset(0, 4), blurRadius: 16 })],
        }),
        child: this.child,
      }),
    });
  }
}

/** 标题 + 内容 + 操作按钮的对话框。 */
export class AlertDialog extends StatelessWidget {
  readonly title: Widget | null;
  readonly content: Widget | null;
  readonly actions: readonly Widget[];
  constructor(o: { key?: Key | null; title?: Widget | null; content?: Widget | null; actions?: readonly Widget[] } = {}) {
    super(o.key);
    this.title = o.title ?? null;
    this.content = o.content ?? null;
    this.actions = o.actions ?? [];
  }
  build(context: BuildContext): Widget {
    const theme = Theme.of(context);
    const children: Widget[] = [];
    if (this.title !== null) children.push(new DefaultTextStyle({ style: theme.textTheme.headlineMedium.copyWith({ fontSize: 24 }), child: this.title }));
    if (this.content !== null) {
      if (children.length > 0) children.push(new SizedBox({ height: 16 }));
      children.push(new DefaultTextStyle({ style: theme.textTheme.bodyMedium, child: this.content }));
    }
    if (this.actions.length > 0) {
      children.push(new SizedBox({ height: 24 }));
      const row: Widget[] = [];
      this.actions.forEach((a, i) => {
        if (i > 0) row.push(new SizedBox({ width: 8 }));
        row.push(a);
      });
      children.push(new Row({ mainAxisAlignment: "end", children: row }));
    }
    return new Dialog({
      child: new Padding({
        padding: EdgeInsets.all(24),
        child: new Column({ mainAxisSize: "min", crossAxisAlignment: "start", children }),
      }),
    });
  }
}
