import type { Key } from "../foundation/key";
import { BorderRadius } from "../painting/borders";
import { Color } from "../painting/color";
import { BoxDecoration, BoxShadow } from "../painting/decoration";
import { EdgeInsets } from "../painting/edge_insets";
import { Offset } from "../painting/geometry";
import type { TextStyle } from "../painting/text_style";
import { ActivateIntent, Actions, CallbackAction } from "../views/actions";
import { Center, ConstrainedBox, Padding } from "../views/basic";
import { BoxConstraints } from "../rendering/box_constraints";
import { Focus } from "../views/focus_scope";
import { FocusNode } from "../views/focus_manager";
import { MouseRegion } from "../views/basic";
import { GestureDetector } from "../views/gesture_detector";
import { State, StatefulView, type BuildContext, type View } from "../views/framework";
import { DefaultTextStyle } from "../views/text";
import { AnimatedContainer } from "../views/implicit_animations";
import { Theme, type ThemeData } from "./theme";

/** 交互状态集合。 */
export interface ButtonStates {
  readonly hovered: boolean;
  readonly pressed: boolean;
  readonly focused: boolean;
  readonly disabled: boolean;
}

/** 按钮外观。 */
export interface ButtonStyle {
  backgroundColor?: (s: ButtonStates) => Color | null;
  foregroundColor?: (s: ButtonStates) => Color;
  padding?: EdgeInsets;
  borderRadius?: BorderRadius;
  elevation?: (s: ButtonStates) => number;
  minimumSize?: { width: number; height: number };
  textStyle?: TextStyle;
}

/** 按钮基类:hover/pressed/focused 状态、键盘激活(Enter/Space)、鼠标光标。 */
export abstract class ButtonStyleButton extends StatefulView {
  readonly onPressed: (() => void) | null;
  readonly onLongPress: (() => void) | null;
  readonly child: View;
  readonly style: ButtonStyle | null;
  readonly focusNode: FocusNode | null;
  readonly autofocus: boolean;
  constructor(o: { key?: Key | null; onPressed: (() => void) | null; onLongPress?: (() => void) | null; child: View; style?: ButtonStyle | null; focusNode?: FocusNode | null; autofocus?: boolean }) {
    super(o.key);
    this.onPressed = o.onPressed;
    this.onLongPress = o.onLongPress ?? null;
    this.child = o.child;
    this.style = o.style ?? null;
    this.focusNode = o.focusNode ?? null;
    this.autofocus = o.autofocus ?? false;
  }
  get enabled(): boolean {
    return this.onPressed !== null || this.onLongPress !== null;
  }
  /** 主题默认样式。 */
  abstract defaultStyle(theme: ThemeData): Required<Omit<ButtonStyle, "textStyle">> & { textStyle: TextStyle };
  createState(): State {
    return new ButtonState();
  }
}

export class ButtonState extends State<ButtonStyleButton> {
  hovered = false;
  pressed = false;
  focused = false;

  get states(): ButtonStates {
    return { hovered: this.hovered, pressed: this.pressed, focused: this.focused, disabled: !this.view.enabled };
  }

  #activate = (): void => {
    this.view.onPressed?.();
  };

  build(context: BuildContext): View {
    const w = this.view;
    const theme = Theme.of(context);
    const d = w.defaultStyle(theme);
    const s = w.style ?? {};
    const st = this.states;
    const bg = (s.backgroundColor ?? d.backgroundColor)(st);
    const fg = (s.foregroundColor ?? d.foregroundColor)(st);
    const elevation = (s.elevation ?? d.elevation)(st);
    const radius = s.borderRadius ?? d.borderRadius;
    const padding = s.padding ?? d.padding;
    const min = s.minimumSize ?? d.minimumSize;
    const textStyle = (s.textStyle ?? d.textStyle).copyWith({ color: fg });
    const enabled = w.enabled;
    return new Actions({
      actions: new Map([[ActivateIntent, new CallbackAction<ActivateIntent>({ onInvoke: () => this.#activate(), isEnabled: () => enabled })]]),
      child: new Focus({
        focusNode: w.focusNode,
        autofocus: w.autofocus,
        canRequestFocus: enabled,
        onFocusChange: (f) => this.setState(() => (this.focused = f)),
        child: new MouseRegion({
          cursor: enabled ? "click" : "basic",
          onEnter: () => this.setState(() => (this.hovered = true)),
          onExit: () => this.setState(() => {
            this.hovered = false;
            this.pressed = false;
          }),
          child: new GestureDetector({
            behavior: "opaque",
            onTapDown: enabled ? () => this.setState(() => (this.pressed = true)) : null,
            onTapUp: enabled ? () => this.setState(() => (this.pressed = false)) : null,
            onTapCancel: enabled ? () => this.setState(() => (this.pressed = false)) : null,
            onTap: w.onPressed,
            onLongPress: w.onLongPress,
            child: new ConstrainedBox({
              constraints: BoxConstraints.from({ minWidth: min.width, minHeight: min.height }),
              child: new AnimatedContainer({
                duration: 120,
                decoration: new BoxDecoration({
                  color: bg,
                  borderRadius: radius,
                  boxShadow: elevation > 0 ? [new BoxShadow({ color: new Color(0x33000000), offset: new Offset(0, elevation / 2), blurRadius: elevation * 1.5 })] : null,
                  border: null,
                }),
                child: new Padding({
                  padding,
                  child: new Center({ widthFactor: 1, heightFactor: 1, child: new DefaultTextStyle({ style: textStyle, child: w.child }) }),
                }),
              }),
            }),
          }),
        }),
      }),
    });
  }
}

function overlay(base: Color, onColor: Color, st: ButtonStates, theme: ThemeData): Color {
  const o = st.pressed ? theme.pressedOpacity : st.hovered ? theme.hoverOpacity : st.focused ? theme.focusOpacity : 0;
  return o > 0 ? Color.alphaBlend(onColor.withOpacity(o), base) : base;
}

/** 有底色与阴影的按钮。 */
export class ElevatedButton extends ButtonStyleButton {
  defaultStyle(theme: ThemeData): Required<Omit<ButtonStyle, "textStyle">> & { textStyle: TextStyle } {
    const cs = theme.colorScheme;
    return {
      backgroundColor: (st) => st.disabled ? cs.onSurface.withOpacity(0.12) : overlay(cs.primary, cs.onPrimary, st, theme),
      foregroundColor: (st) => st.disabled ? cs.onSurface.withOpacity(0.38) : cs.onPrimary,
      padding: EdgeInsets.symmetric({ horizontal: 24, vertical: 10 }),
      borderRadius: BorderRadius.circular(20),
      elevation: (st) => st.disabled ? 0 : st.pressed ? 1 : st.hovered ? 3 : 1,
      minimumSize: { width: 64, height: 40 },
      textStyle: theme.textTheme.labelLarge,
    };
  }
}

/** 文字按钮(无底色, hover/pressed 时显示浅色叠加)。 */
export class TextButton extends ButtonStyleButton {
  defaultStyle(theme: ThemeData): Required<Omit<ButtonStyle, "textStyle">> & { textStyle: TextStyle } {
    const cs = theme.colorScheme;
    return {
      backgroundColor: (st) => {
        const o = st.disabled ? 0 : st.pressed ? theme.pressedOpacity : st.hovered ? theme.hoverOpacity : st.focused ? theme.focusOpacity : 0;
        return o > 0 ? cs.primary.withOpacity(o) : null;
      },
      foregroundColor: (st) => st.disabled ? cs.onSurface.withOpacity(0.38) : cs.primary,
      padding: EdgeInsets.symmetric({ horizontal: 12, vertical: 8 }),
      borderRadius: BorderRadius.circular(20),
      elevation: () => 0,
      minimumSize: { width: 48, height: 36 },
      textStyle: theme.textTheme.labelLarge,
    };
  }
}

/** 浮动操作按钮(圆形)。 */
export class FloatingActionButton extends ButtonStyleButton {
  defaultStyle(theme: ThemeData): Required<Omit<ButtonStyle, "textStyle">> & { textStyle: TextStyle } {
    const cs = theme.colorScheme;
    return {
      backgroundColor: (st) => overlay(cs.primary, cs.onPrimary, st, theme),
      foregroundColor: () => cs.onPrimary,
      padding: EdgeInsets.all(16),
      borderRadius: BorderRadius.circular(16),
      elevation: (st) => st.pressed ? 4 : st.hovered ? 8 : 6,
      minimumSize: { width: 56, height: 56 },
      textStyle: theme.textTheme.titleLarge,
    };
  }
}
