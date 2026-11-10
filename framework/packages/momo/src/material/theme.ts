import type { Key } from "../foundation/key";
import { Color, Colors } from "../painting/color";
import { TextStyle, FontWeight } from "../painting/text_style";
import { InheritedWidget, StatelessWidget, type BuildContext, type Widget } from "../widgets/framework";
import { DefaultTextStyle } from "../widgets/text";

/** 明暗。 */
export type Brightness = "light" | "dark";

/** 配色(Material 3 子集)。 */
export class ColorScheme {
  readonly brightness: Brightness;
  readonly primary: Color;
  readonly onPrimary: Color;
  readonly secondary: Color;
  readonly onSecondary: Color;
  readonly surface: Color;
  readonly onSurface: Color;
  readonly background: Color;
  readonly onBackground: Color;
  readonly error: Color;
  readonly onError: Color;
  readonly outline: Color;
  constructor(o: { brightness?: Brightness; primary: Color; onPrimary?: Color; secondary?: Color; onSecondary?: Color; surface?: Color; onSurface?: Color;
    background?: Color; onBackground?: Color; error?: Color; onError?: Color; outline?: Color }) {
    const dark = o.brightness === "dark";
    this.brightness = o.brightness ?? "light";
    this.primary = o.primary;
    this.onPrimary = o.onPrimary ?? Colors.white;
    this.secondary = o.secondary ?? o.primary;
    this.onSecondary = o.onSecondary ?? Colors.white;
    this.surface = o.surface ?? (dark ? new Color(0xff1e1e1e) : Colors.white);
    this.onSurface = o.onSurface ?? (dark ? Colors.white : new Color(0xff1c1b1f));
    this.background = o.background ?? (dark ? new Color(0xff121212) : new Color(0xfffafafa));
    this.onBackground = o.onBackground ?? this.onSurface;
    this.error = o.error ?? new Color(0xffb3261e);
    this.onError = o.onError ?? Colors.white;
    this.outline = o.outline ?? (dark ? new Color(0xff938f99) : new Color(0xff79747e));
  }
  /** 由种子色生成。 */
  static fromSeed(seed: Color, brightness: Brightness = "light"): ColorScheme {
    return new ColorScheme({ brightness, primary: seed });
  }
}

/** 文本主题。 */
export class TextTheme {
  readonly titleLarge: TextStyle;
  readonly titleMedium: TextStyle;
  readonly bodyLarge: TextStyle;
  readonly bodyMedium: TextStyle;
  readonly labelLarge: TextStyle;
  readonly headlineMedium: TextStyle;
  constructor(color: Color) {
    this.headlineMedium = new TextStyle({ fontSize: 28, color });
    this.titleLarge = new TextStyle({ fontSize: 22, fontWeight: FontWeight.w500, color });
    this.titleMedium = new TextStyle({ fontSize: 16, fontWeight: FontWeight.w500, color });
    this.bodyLarge = new TextStyle({ fontSize: 16, color });
    this.bodyMedium = new TextStyle({ fontSize: 14, color });
    this.labelLarge = new TextStyle({ fontSize: 14, fontWeight: FontWeight.w500, color });
  }
}

/** 主题数据。 */
export class ThemeData {
  readonly colorScheme: ColorScheme;
  readonly textTheme: TextTheme;
  readonly scaffoldBackgroundColor: Color;
  readonly dividerColor: Color;
  readonly hoverOpacity: number;
  readonly pressedOpacity: number;
  readonly focusOpacity: number;
  constructor(o: { colorScheme?: ColorScheme; primaryColor?: Color; brightness?: Brightness; scaffoldBackgroundColor?: Color } = {}) {
    this.colorScheme = o.colorScheme ?? new ColorScheme({ primary: o.primaryColor ?? new Color(0xff6750a4), brightness: o.brightness ?? "light" });
    this.textTheme = new TextTheme(this.colorScheme.onSurface);
    this.scaffoldBackgroundColor = o.scaffoldBackgroundColor ?? this.colorScheme.background;
    this.dividerColor = this.colorScheme.outline.withOpacity(0.3);
    this.hoverOpacity = 0.08;
    this.pressedOpacity = 0.12;
    this.focusOpacity = 0.12;
  }
  get primaryColor(): Color {
    return this.colorScheme.primary;
  }
  get brightness(): Brightness {
    return this.colorScheme.brightness;
  }
  static light(): ThemeData {
    return new ThemeData();
  }
  static dark(): ThemeData {
    return new ThemeData({ brightness: "dark", primaryColor: new Color(0xffd0bcff) });
  }
}

const defaultTheme = new ThemeData();

class InheritedTheme extends InheritedWidget {
  constructor(readonly data: ThemeData, child: Widget) {
    super({ child });
  }
  updateShouldNotify(old: InheritedTheme): boolean {
    return old.data !== this.data;
  }
}

/** 向子树提供主题(并设置默认文本样式)。 */
export class Theme extends StatelessWidget {
  readonly data: ThemeData;
  readonly child: Widget;
  constructor(o: { key?: Key | null; data: ThemeData; child: Widget }) {
    super(o.key);
    this.data = o.data;
    this.child = o.child;
  }
  static of(context: BuildContext): ThemeData {
    return context.dependOnInheritedWidgetOfExactType(InheritedTheme)?.data ?? defaultTheme;
  }
  build(): Widget {
    return new InheritedTheme(this.data, new DefaultTextStyle({ style: this.data.textTheme.bodyMedium, child: this.child }));
  }
}
