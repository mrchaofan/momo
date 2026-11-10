import { clampDouble, lerpDouble } from "./geometry";

/** 32 位 ARGB 颜色(非预乘), 与 momo:ui 的 Color(0xAARRGGBB 数字)一致。 */
export class Color {
  /** 0xAARRGGBB。 */
  readonly value: number;
  constructor(value: number) {
    this.value = value >>> 0;
  }
  static fromARGB(a: number, r: number, g: number, b: number): Color {
    return new Color((((a & 0xff) << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff)) >>> 0);
  }
  /** opacity 为 0..1。 */
  static fromRGBO(r: number, g: number, b: number, opacity: number): Color {
    return Color.fromARGB(Math.round(clampDouble(opacity, 0, 1) * 255), r, g, b);
  }
  get alpha(): number {
    return (this.value >>> 24) & 0xff;
  }
  get red(): number {
    return (this.value >>> 16) & 0xff;
  }
  get green(): number {
    return (this.value >>> 8) & 0xff;
  }
  get blue(): number {
    return this.value & 0xff;
  }
  get opacity(): number {
    return this.alpha / 255;
  }
  withAlpha(a: number): Color {
    return Color.fromARGB(a, this.red, this.green, this.blue);
  }
  withOpacity(opacity: number): Color {
    return this.withAlpha(Math.round(clampDouble(opacity, 0, 1) * 255));
  }
  /** 相对亮度(0..1, sRGB → 线性)。 */
  computeLuminance(): number {
    const lin = (c: number): number => {
      const v = c / 255;
      return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(this.red) + 0.7152 * lin(this.green) + 0.0722 * lin(this.blue);
  }
  equals(other: Color | null | undefined): boolean {
    return other != null && other.value === this.value;
  }
  /** 分量线性插值。 */
  static lerp(a: Color | null, b: Color | null, t: number): Color | null {
    if (a === null && b === null) return null;
    const x = a ?? Colors.transparent;
    const y = b ?? Colors.transparent;
    const ch = (p: number, q: number): number => clampDouble(Math.round(lerpDouble(p, q, t)), 0, 255);
    return Color.fromARGB(ch(x.alpha, y.alpha), ch(x.red, y.red), ch(x.green, y.green), ch(x.blue, y.blue));
  }
  /** 在 background 上叠加 foreground(alpha 合成)。 */
  static alphaBlend(foreground: Color, background: Color): Color {
    const a = foreground.alpha;
    if (a === 0) return background;
    const inv = 255 - a;
    const ba = background.alpha;
    if (ba === 255) {
      return Color.fromARGB(255, (a * foreground.red + inv * background.red) / 255 | 0,
        (a * foreground.green + inv * background.green) / 255 | 0, (a * foreground.blue + inv * background.blue) / 255 | 0);
    }
    const outA = a + (ba * inv) / 255;
    const mix = (f: number, bg: number): number => ((f * a + (bg * ba * inv) / 255) / outA) | 0;
    return Color.fromARGB(outA | 0, mix(foreground.red, background.red), mix(foreground.green, background.green), mix(foreground.blue, background.blue));
  }
  toString(): string {
    return `Color(0x${this.value.toString(16).padStart(8, "0")})`;
  }
}

/** 常用颜色(Material 调色板子集)。 */
export const Colors = {
  transparent: new Color(0x00000000),
  black: new Color(0xff000000),
  black87: new Color(0xdd000000),
  black54: new Color(0x8a000000),
  black45: new Color(0x73000000),
  black38: new Color(0x61000000),
  black26: new Color(0x42000000),
  black12: new Color(0x1f000000),
  white: new Color(0xffffffff),
  white70: new Color(0xb3ffffff),
  white54: new Color(0x8affffff),
  white38: new Color(0x62ffffff),
  white24: new Color(0x3dffffff),
  white12: new Color(0x1fffffff),
  red: new Color(0xfff44336),
  pink: new Color(0xffe91e63),
  purple: new Color(0xff9c27b0),
  deepPurple: new Color(0xff673ab7),
  indigo: new Color(0xff3f51b5),
  blue: new Color(0xff2196f3),
  lightBlue: new Color(0xff03a9f4),
  cyan: new Color(0xff00bcd4),
  teal: new Color(0xff009688),
  green: new Color(0xff4caf50),
  lightGreen: new Color(0xff8bc34a),
  lime: new Color(0xffcddc39),
  yellow: new Color(0xffffeb3b),
  amber: new Color(0xffffc107),
  orange: new Color(0xffff9800),
  deepOrange: new Color(0xffff5722),
  brown: new Color(0xff795548),
  grey: new Color(0xff9e9e9e),
  grey100: new Color(0xfff5f5f5),
  grey200: new Color(0xffeeeeee),
  grey300: new Color(0xffe0e0e0),
  grey400: new Color(0xffbdbdbd),
  grey600: new Color(0xff757575),
  grey700: new Color(0xff616161),
  grey800: new Color(0xff424242),
  grey900: new Color(0xff212121),
  blueGrey: new Color(0xff607d8b),
} as const;
