import type {
  FontStyle, TextBaseline, TextDecoration, TextDecorationStyle, TextStyle as UiTextStyle,
} from "momo:ui";
import { Color } from "./color";
import { lerpDouble } from "./geometry";

/** 字重常量(100..900)。 */
export const FontWeight = {
  w100: 100, w200: 200, w300: 300, w400: 400, w500: 500, w600: 600, w700: 700, w800: 800, w900: 900,
  normal: 400, bold: 700,
} as const;

/** TextStyle 构造参数。 */
export interface TextStyleOptions {
  /** 为 true(默认)时与祖先 DefaultTextStyle 合并。 */
  inherit?: boolean;
  color?: Color;
  backgroundColor?: Color;
  fontSize?: number;
  fontWeight?: number;
  fontStyle?: FontStyle;
  letterSpacing?: number;
  wordSpacing?: number;
  /** 行高倍数。 */
  height?: number;
  fontFamily?: string;
  fontFamilyFallback?: readonly string[];
  decoration?: TextDecoration;
  decorationColor?: Color;
  decorationStyle?: TextDecorationStyle;
  decorationThickness?: number;
  textBaseline?: TextBaseline;
  locale?: string;
}

/** 文本样式(不可变)。未设置的字段在合并时由外层样式提供。 */
export class TextStyle {
  readonly inherit: boolean;
  readonly color: Color | undefined;
  readonly backgroundColor: Color | undefined;
  readonly fontSize: number | undefined;
  readonly fontWeight: number | undefined;
  readonly fontStyle: FontStyle | undefined;
  readonly letterSpacing: number | undefined;
  readonly wordSpacing: number | undefined;
  readonly height: number | undefined;
  readonly fontFamily: string | undefined;
  readonly fontFamilyFallback: readonly string[] | undefined;
  readonly decoration: TextDecoration | undefined;
  readonly decorationColor: Color | undefined;
  readonly decorationStyle: TextDecorationStyle | undefined;
  readonly decorationThickness: number | undefined;
  readonly textBaseline: TextBaseline | undefined;
  readonly locale: string | undefined;
  #key: string | null = null;
  #ui: UiTextStyle | null = null;

  constructor(o: TextStyleOptions = {}) {
    this.inherit = o.inherit ?? true;
    this.color = o.color;
    this.backgroundColor = o.backgroundColor;
    this.fontSize = o.fontSize;
    this.fontWeight = o.fontWeight;
    this.fontStyle = o.fontStyle;
    this.letterSpacing = o.letterSpacing;
    this.wordSpacing = o.wordSpacing;
    this.height = o.height;
    this.fontFamily = o.fontFamily;
    this.fontFamilyFallback = o.fontFamilyFallback;
    this.decoration = o.decoration;
    this.decorationColor = o.decorationColor;
    this.decorationStyle = o.decorationStyle;
    this.decorationThickness = o.decorationThickness;
    this.textBaseline = o.textBaseline;
    this.locale = o.locale;
  }

  #options(): TextStyleOptions {
    const o: TextStyleOptions = { inherit: this.inherit };
    if (this.color !== undefined) o.color = this.color;
    if (this.backgroundColor !== undefined) o.backgroundColor = this.backgroundColor;
    if (this.fontSize !== undefined) o.fontSize = this.fontSize;
    if (this.fontWeight !== undefined) o.fontWeight = this.fontWeight;
    if (this.fontStyle !== undefined) o.fontStyle = this.fontStyle;
    if (this.letterSpacing !== undefined) o.letterSpacing = this.letterSpacing;
    if (this.wordSpacing !== undefined) o.wordSpacing = this.wordSpacing;
    if (this.height !== undefined) o.height = this.height;
    if (this.fontFamily !== undefined) o.fontFamily = this.fontFamily;
    if (this.fontFamilyFallback !== undefined) o.fontFamilyFallback = this.fontFamilyFallback;
    if (this.decoration !== undefined) o.decoration = this.decoration;
    if (this.decorationColor !== undefined) o.decorationColor = this.decorationColor;
    if (this.decorationStyle !== undefined) o.decorationStyle = this.decorationStyle;
    if (this.decorationThickness !== undefined) o.decorationThickness = this.decorationThickness;
    if (this.textBaseline !== undefined) o.textBaseline = this.textBaseline;
    if (this.locale !== undefined) o.locale = this.locale;
    return o;
  }

  /** 复制并覆盖部分字段。 */
  copyWith(o: TextStyleOptions): TextStyle {
    const base = this.#options();
    for (const [k, v] of Object.entries(o)) {
      if (v !== undefined) (base as Record<string, unknown>)[k] = v;
    }
    return new TextStyle(base);
  }

  /** other 中已设置的字段覆盖本样式;other.inherit=false 时直接返回 other。 */
  merge(other: TextStyle | null | undefined): TextStyle {
    if (!other) return this;
    if (!other.inherit) return other;
    const o = other.#options();
    delete o.inherit;
    return this.copyWith(o);
  }

  /** 字号缩放等派生。 */
  apply(o: { fontSizeFactor?: number; color?: Color }): TextStyle {
    return this.copyWith({
      ...(o.color !== undefined ? { color: o.color } : {}),
      ...(this.fontSize !== undefined && o.fontSizeFactor !== undefined ? { fontSize: this.fontSize * o.fontSizeFactor } : {}),
    });
  }

  /** 转为 momo:ui 的 TextStyle(只包含已设置字段, 便于 pushStyle 叠加)。 */
  toUi(): UiTextStyle {
    if (this.#ui !== null) return this.#ui;
    const s: UiTextStyle = {};
    if (this.color !== undefined) s.color = this.color.value;
    if (this.backgroundColor !== undefined) s.backgroundColor = this.backgroundColor.value;
    if (this.fontSize !== undefined) s.fontSize = this.fontSize;
    if (this.fontWeight !== undefined) s.fontWeight = this.fontWeight;
    if (this.fontStyle !== undefined) s.fontStyle = this.fontStyle;
    if (this.letterSpacing !== undefined) s.letterSpacing = this.letterSpacing;
    if (this.wordSpacing !== undefined) s.wordSpacing = this.wordSpacing;
    if (this.height !== undefined) s.height = this.height;
    if (this.fontFamily !== undefined || this.fontFamilyFallback !== undefined) {
      s.fontFamilies = [...(this.fontFamily !== undefined ? [this.fontFamily] : []), ...(this.fontFamilyFallback ?? [])];
    }
    if (this.decoration !== undefined) s.decoration = this.decoration;
    if (this.decorationColor !== undefined) s.decorationColor = this.decorationColor.value;
    if (this.decorationStyle !== undefined) s.decorationStyle = this.decorationStyle;
    if (this.decorationThickness !== undefined) s.decorationThickness = this.decorationThickness;
    if (this.textBaseline !== undefined) s.textBaseline = this.textBaseline;
    if (this.locale !== undefined) s.locale = this.locale;
    this.#ui = s;
    return s;
  }

  /** 缓存键(值语义)。 */
  get cacheKey(): string {
    if (this.#key === null) this.#key = JSON.stringify(this.toUi());
    return this.#key;
  }

  equals(other: TextStyle | null | undefined): boolean {
    return other != null && (other === this || (other.inherit === this.inherit && other.cacheKey === this.cacheKey));
  }

  /** 线性插值(数值字段插值, 其余在 t<0.5 取 a)。 */
  static lerp(a: TextStyle | null, b: TextStyle | null, t: number): TextStyle | null {
    if (a === null || b === null) return t < 0.5 ? a : b;
    const pick = t < 0.5 ? a : b;
    const num = (x: number | undefined, y: number | undefined): number | undefined =>
      x !== undefined && y !== undefined ? lerpDouble(x, y, t) : t < 0.5 ? x : y;
    const color = a.color && b.color ? Color.lerp(a.color, b.color, t) ?? undefined : pick.color;
    const opts: TextStyleOptions = { ...pick.#options() };
    const fs = num(a.fontSize, b.fontSize);
    if (fs !== undefined) opts.fontSize = fs;
    const ls = num(a.letterSpacing, b.letterSpacing);
    if (ls !== undefined) opts.letterSpacing = ls;
    if (color !== undefined) opts.color = color;
    return new TextStyle(opts);
  }
}
