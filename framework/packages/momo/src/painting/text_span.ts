import type { ParagraphBuilder } from "momo:ui";
import { TextStyle } from "./text_style";

/** 富文本片段树(对标 Flutter TextSpan)。 */
export class TextSpan {
  readonly text: string | null;
  readonly style: TextStyle | null;
  readonly children: readonly TextSpan[] | null;
  #key: string | null = null;
  #plain: string | null = null;

  constructor(o: { text?: string | null; style?: TextStyle | null; children?: readonly TextSpan[] | null } = {}) {
    this.text = o.text ?? null;
    this.style = o.style ?? null;
    this.children = o.children ?? null;
  }

  /** 写入 ParagraphBuilder。 */
  build(builder: ParagraphBuilder): void {
    const hasStyle = this.style !== null;
    if (hasStyle) builder.pushStyle(this.style!.toUi());
    if (this.text !== null) builder.addText(this.text);
    if (this.children) for (const c of this.children) c.build(builder);
    if (hasStyle) builder.pop();
  }

  /** 纯文本。 */
  toPlainText(): string {
    if (this.#plain === null) {
      let s = this.text ?? "";
      if (this.children) for (const c of this.children) s += c.toPlainText();
      this.#plain = s;
    }
    return this.#plain;
  }

  /** 值语义缓存键。 */
  get cacheKey(): string {
    if (this.#key === null) {
      let k = `(${this.style ? this.style.cacheKey : ""}|${this.text === null ? "" : JSON.stringify(this.text)}`;
      if (this.children) for (const c of this.children) k += c.cacheKey;
      this.#key = k + ")";
    }
    return this.#key;
  }

  /** 遍历(含自身)。返回 false 停止。 */
  visit(visitor: (span: TextSpan) => boolean): boolean {
    if (!visitor(this)) return false;
    if (this.children) for (const c of this.children) if (!c.visit(visitor)) return false;
    return true;
  }

  equals(other: TextSpan | null | undefined): boolean {
    return other != null && (other === this || other.cacheKey === this.cacheKey);
  }
}
