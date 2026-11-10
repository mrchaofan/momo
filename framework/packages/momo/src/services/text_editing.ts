/** 文本位置的亲和性(软折行处光标属于上一行末尾还是下一行开头)。 */
export type TextAffinity = "downstream" | "upstream";

/** UTF-16 码元区间 [start, end)。(-1, -1) 表示无效/空。 */
export class TextRange {
  static readonly empty = new TextRange(-1, -1);
  constructor(readonly start: number, readonly end: number) {}
  static collapsed(offset: number): TextRange {
    return new TextRange(offset, offset);
  }
  get isValid(): boolean {
    return this.start >= 0 && this.end >= 0;
  }
  get isCollapsed(): boolean {
    return this.start === this.end;
  }
  get isNormalized(): boolean {
    return this.end >= this.start;
  }
  textBefore(text: string): string {
    return text.substring(0, this.start);
  }
  textAfter(text: string): string {
    return text.substring(this.end);
  }
  textInside(text: string): string {
    return text.substring(this.start, this.end);
  }
  equals(other: TextRange | null | undefined): boolean {
    return other != null && other.start === this.start && other.end === this.end;
  }
  toString(): string {
    return `TextRange(${this.start}, ${this.end})`;
  }
}

/** 文本选区:base(锚点)与 extent(活动端), start/end 为二者的 min/max。 */
export class TextSelection extends TextRange {
  constructor(readonly baseOffset: number, readonly extentOffset: number, readonly affinity: TextAffinity = "downstream") {
    super(Math.min(baseOffset, extentOffset), Math.max(baseOffset, extentOffset));
  }
  static override collapsed(offset: number, affinity: TextAffinity = "downstream"): TextSelection {
    return new TextSelection(offset, offset, affinity);
  }
  static fromPosition(position: { offset: number; affinity?: TextAffinity }): TextSelection {
    return new TextSelection(position.offset, position.offset, position.affinity ?? "downstream");
  }
  /** 选区是否向后(base ≤ extent)。 */
  get isDirectional(): boolean {
    return this.baseOffset <= this.extentOffset;
  }
  copyWith(o: { baseOffset?: number; extentOffset?: number; affinity?: TextAffinity }): TextSelection {
    return new TextSelection(o.baseOffset ?? this.baseOffset, o.extentOffset ?? this.extentOffset, o.affinity ?? this.affinity);
  }
  /** 把 extent 移到 offset(保留 base)。 */
  extendTo(offset: number): TextSelection {
    return new TextSelection(this.baseOffset, offset, this.affinity);
  }
  override equals(other: TextRange | null | undefined): boolean {
    return other instanceof TextSelection && other.baseOffset === this.baseOffset &&
      other.extentOffset === this.extentOffset && other.affinity === this.affinity;
  }
  override toString(): string {
    return `TextSelection(base: ${this.baseOffset}, extent: ${this.extentOffset})`;
  }
}

/** 编辑状态:文本、选区、composing(输入法预编辑)区间。 */
export class TextEditingValue {
  static readonly empty = new TextEditingValue();
  readonly text: string;
  readonly selection: TextSelection;
  readonly composing: TextRange;

  constructor(o: { text?: string; selection?: TextSelection; composing?: TextRange } = {}) {
    this.text = o.text ?? "";
    this.selection = o.selection ?? TextSelection.collapsed(-1);
    this.composing = o.composing ?? TextRange.empty;
  }

  copyWith(o: { text?: string; selection?: TextSelection; composing?: TextRange }): TextEditingValue {
    return new TextEditingValue({
      text: o.text ?? this.text,
      selection: o.selection ?? this.selection,
      composing: o.composing ?? this.composing,
    });
  }

  /** composing 区间是否有效且在文本内。 */
  get isComposingRangeValid(): boolean {
    return this.composing.isValid && this.composing.isNormalized && this.composing.end <= this.text.length;
  }

  /**
   * 把 range 替换为 replacement:选区两端按替换修正(落在 range 内的移到替换后末尾),composing 清空。
   */
  replaced(range: TextRange, replacement: string): TextEditingValue {
    const text = this.text.substring(0, range.start) + replacement + this.text.substring(range.end);
    const delta = replacement.length - (range.end - range.start);
    const adjust = (o: number): number => {
      if (o < range.start) return o;
      if (o >= range.end) return o + delta;
      return range.start + replacement.length;
    };
    const sel = this.selection.isValid
      ? new TextSelection(adjust(this.selection.baseOffset), adjust(this.selection.extentOffset), this.selection.affinity)
      : this.selection;
    return new TextEditingValue({ text, selection: sel, composing: TextRange.empty });
  }

  equals(other: TextEditingValue | null | undefined): boolean {
    return other != null && other.text === this.text && other.selection.equals(this.selection) && other.composing.equals(this.composing);
  }

  toString(): string {
    return `TextEditingValue(text: ${JSON.stringify(this.text)}, selection: ${this.selection}, composing: ${this.composing})`;
  }
}
