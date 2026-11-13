import type { Key } from "../foundation/key";
import { Border, BorderRadius } from "../painting/borders";
import type { Color } from "../painting/color";
import { BoxDecoration } from "../painting/decoration";
import { EdgeInsets } from "../painting/edge_insets";
import type { TextStyle } from "../painting/text_style";
import { Alignment } from "../painting/alignment";
import { IgnorePointer, MouseRegion, Stack } from "../views/basic";
import { Container } from "../views/container";
import { EditableText, TextEditingController } from "../views/editable_text";
import { FocusNode } from "../views/focus_manager";
import { State, StatefulView, type BuildContext, type View } from "../views/framework";
import { Text } from "../views/text";
import { Theme } from "./theme";

/** 输入框装饰(简化版)。 */
export class InputDecoration {
  readonly hintText: string | null;
  readonly contentPadding: EdgeInsets;
  readonly filled: boolean;
  readonly fillColor: Color | null;
  readonly borderRadius: BorderRadius;
  constructor(o: { hintText?: string | null; contentPadding?: EdgeInsets; filled?: boolean; fillColor?: Color | null; borderRadius?: BorderRadius } = {}) {
    this.hintText = o.hintText ?? null;
    this.contentPadding = o.contentPadding ?? EdgeInsets.symmetric({ horizontal: 12, vertical: 10 });
    this.filled = o.filled ?? false;
    this.fillColor = o.fillColor ?? null;
    this.borderRadius = o.borderRadius ?? BorderRadius.circular(4);
  }
}

/** 文本输入框(基于 EditableText):边框随 hover/focus 变化, 空文本显示提示。 */
export class TextField extends StatefulView {
  readonly controller: TextEditingController | null;
  readonly focusNode: FocusNode | null;
  readonly decoration: InputDecoration;
  readonly style: TextStyle | null;
  readonly obscureText: boolean;
  readonly maxLines: number | null;
  readonly autofocus: boolean;
  readonly enabled: boolean;
  readonly onChanged: ((text: string) => void) | null;
  readonly onSubmitted: ((text: string) => void) | null;
  readonly cursorBlink: boolean;
  constructor(o: { key?: Key | null; controller?: TextEditingController | null; focusNode?: FocusNode | null; decoration?: InputDecoration; style?: TextStyle | null;
    obscureText?: boolean; maxLines?: number | null; autofocus?: boolean; enabled?: boolean; onChanged?: ((text: string) => void) | null;
    onSubmitted?: ((text: string) => void) | null; cursorBlink?: boolean } = {}) {
    super(o.key);
    this.controller = o.controller ?? null;
    this.focusNode = o.focusNode ?? null;
    this.decoration = o.decoration ?? new InputDecoration();
    this.style = o.style ?? null;
    this.obscureText = o.obscureText ?? false;
    this.maxLines = o.maxLines === undefined ? 1 : o.maxLines;
    this.autofocus = o.autofocus ?? false;
    this.enabled = o.enabled ?? true;
    this.onChanged = o.onChanged ?? null;
    this.onSubmitted = o.onSubmitted ?? null;
    this.cursorBlink = o.cursorBlink ?? true;
  }
  createState(): State {
    return new TextFieldState();
  }
}

export class TextFieldState extends State<TextField> {
  #controller: TextEditingController | null = null;
  #focusNode: FocusNode | null = null;
  #hovered = false;

  get controller(): TextEditingController {
    return this.view.controller ?? (this.#controller ??= new TextEditingController());
  }
  get focusNode(): FocusNode {
    return this.view.focusNode ?? (this.#focusNode ??= new FocusNode({ debugLabel: "TextField" }));
  }
  #rebuild = (): void => {
    if (this.mounted) this.setState();
  };
  override initState(): void {
    this.focusNode.addListener(this.#rebuild);
    this.controller.addListener(this.#rebuild);
  }
  override didUpdateView(old: TextField): void {
    const oldNode = old.focusNode ?? this.#focusNode;
    if (oldNode !== this.focusNode) {
      oldNode?.removeListener(this.#rebuild);
      this.focusNode.addListener(this.#rebuild);
    }
    const oldCtl = old.controller ?? this.#controller;
    if (oldCtl !== this.controller) {
      oldCtl?.removeListener(this.#rebuild);
      this.controller.addListener(this.#rebuild);
    }
  }
  override dispose(): void {
    this.focusNode.removeListener(this.#rebuild);
    this.controller.removeListener(this.#rebuild);
    this.#controller?.dispose();
    this.#focusNode?.dispose();
  }

  build(context: BuildContext): View {
    const theme = Theme.of(context);
    const cs = theme.colorScheme;
    const w = this.view;
    const d = w.decoration;
    const focused = this.focusNode.hasFocus;
    const borderColor = !w.enabled ? cs.onSurface.withOpacity(0.12) : focused ? cs.primary : this.#hovered ? cs.onSurface : cs.outline;
    const style = theme.textTheme.bodyLarge.merge(w.style);
    const editable = new EditableText({
      controller: this.controller,
      focusNode: this.focusNode,
      style,
      cursorColor: cs.primary,
      selectionColor: cs.primary.withOpacity(0.3),
      obscureText: w.obscureText,
      maxLines: w.maxLines,
      autofocus: w.autofocus,
      readOnly: !w.enabled,
      cursorBlink: w.cursorBlink,
      onChanged: w.onChanged,
      onSubmitted: w.onSubmitted,
    });
    const children: View[] = [];
    if (d.hintText !== null && this.controller.text.length === 0 && !this.controller.value.isComposingRangeValid) {
      children.push(new IgnorePointer({ child: new Text(d.hintText, { style: style.copyWith({ color: cs.onSurface.withOpacity(0.5) }), maxLines: 1 }) }));
    }
    children.push(editable);
    return new MouseRegion({
      onEnter: () => this.setState(() => (this.#hovered = true)),
      onExit: () => this.setState(() => (this.#hovered = false)),
      child: new IgnorePointer({
        ignoring: !w.enabled,
        child: new Container({
          padding: focused ? d.contentPadding.subtract(EdgeInsets.all(1)) : d.contentPadding,
          decoration: new BoxDecoration({
            color: d.filled ? d.fillColor ?? cs.onSurface.withOpacity(0.04) : null,
            borderRadius: d.borderRadius,
            border: Border.all({ color: borderColor, width: focused ? 2 : 1 }),
          }),
          child: new Stack({ alignment: Alignment.centerLeft, children }),
        }),
      }),
    });
  }
}
