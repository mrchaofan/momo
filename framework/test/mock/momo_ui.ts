// momo:ui 的纯 TS mock(Node 宿主下替代引擎原生模块)。
// - Canvas/SceneBuilder 记录调用;
// - window 可模拟 vsync(由 WidgetTester 直接调用 onBeginFrame/onDrawFrame);
// - Paragraph 采用 Ahem 固定宽度字形模型:每个 UTF-16 码元宽 = 字号, 行高 = 字号 × height(默认 1),
//   ascent = 0.8 × 字号, descent = 0.2 × 字号;按空格/换行贪心折行。
import type { MockControl, RecordedOp } from "../host";

export type Color = number;
export type Matrix4 = ArrayLike<number>;
export type PaintingStyle = "fill" | "stroke";
export type StrokeCap = "butt" | "round" | "square";
export type StrokeJoin = "miter" | "round" | "bevel";
export type TileMode = "clamp" | "repeat" | "mirror";
export type FillType = "nonzero" | "evenodd";
export type FilterQuality = "none" | "low" | "medium" | "high";

const control: MockControl = {
  paragraphsBuilt: 0,
  paragraphLayouts: 0,
  frameRequests: 0,
  renderedScenes: 0,
  lastSceneOps: [],
  cursor: "basic",
  title: "",
  assets: new Map<string, Uint8Array>(),
  decodedImages: 0,
  setPhysicalSize(width: number, height: number, dpr: number): void {
    win.physicalSize = { width, height };
    win.devicePixelRatio = dpr;
  },
  pictureOps(picture: unknown): RecordedOp[] {
    return picture instanceof Picture ? picture.ops : [];
  },
  sceneOpsByView: new Map<number, RecordedOp[]>(),
  titles: new Map<number, string>(),
  openViews(): number[] {
    return [...viewMap.keys()].filter((id) => id !== 0);
  },
  requestCloseView(id: number): boolean {
    const v = viewMap.get(id);
    if (v === undefined || id === 0) return false;
    let allow = true;
    try {
      allow = v.onCloseRequested?.() !== false;
    } catch {
      allow = true;
    }
    if (allow) removeView(id);
    return allow;
  },
  failNextCreateView: false,
  alwaysOnTop: new Map<number, boolean>(),
  activated: [],
  titleBars: new Map<number, { style: string; center: number | null }>(),
  dragRegions: new Map<number, number[]>(),
};
export const __mock: MockControl = control;

export class Gradient {
  private constructor(readonly kind: string, readonly args: unknown[]) {}
  static linear(x0: number, y0: number, x1: number, y1: number, colors: Color[],
                stops?: number[] | null, tileMode?: TileMode): Gradient {
    return new Gradient("linear", [x0, y0, x1, y1, colors, stops, tileMode]);
  }
  static radial(cx: number, cy: number, radius: number, colors: Color[],
                stops?: number[] | null, tileMode?: TileMode): Gradient {
    return new Gradient("radial", [cx, cy, radius, colors, stops, tileMode]);
  }
}

export class Paint {
  color: Color = 0xff000000;
  style: PaintingStyle = "fill";
  strokeWidth = 0;
  strokeCap: StrokeCap = "butt";
  strokeJoin: StrokeJoin = "miter";
  antiAlias = true;
  shader: Gradient | null = null;
  filterQuality: FilterQuality = "low";
  maskBlurSigma = 0;
}

function snapshotPaint(p: Paint | null | undefined): Record<string, unknown> | null {
  if (!p) return null;
  return { color: p.color, style: p.style, strokeWidth: p.strokeWidth, maskBlurSigma: p.maskBlurSigma, shader: p.shader };
}

export class Path {
  fillType: FillType = "nonzero";
  readonly commands: unknown[][] = [];
  #minX = Infinity; #minY = Infinity; #maxX = -Infinity; #maxY = -Infinity;
  #pt(x: number, y: number): void {
    this.#minX = Math.min(this.#minX, x); this.#minY = Math.min(this.#minY, y);
    this.#maxX = Math.max(this.#maxX, x); this.#maxY = Math.max(this.#maxY, y);
  }
  get bounds(): { x: number; y: number; width: number; height: number } {
    if (this.#minX === Infinity) return { x: 0, y: 0, width: 0, height: 0 };
    return { x: this.#minX, y: this.#minY, width: this.#maxX - this.#minX, height: this.#maxY - this.#minY };
  }
  moveTo(x: number, y: number): this { this.commands.push(["moveTo", x, y]); this.#pt(x, y); return this; }
  lineTo(x: number, y: number): this { this.commands.push(["lineTo", x, y]); this.#pt(x, y); return this; }
  quadTo(cx: number, cy: number, x: number, y: number): this { this.commands.push(["quadTo", cx, cy, x, y]); this.#pt(cx, cy); this.#pt(x, y); return this; }
  cubicTo(a: number, b: number, c: number, d: number, x: number, y: number): this { this.commands.push(["cubicTo", a, b, c, d, x, y]); this.#pt(a, b); this.#pt(c, d); this.#pt(x, y); return this; }
  close(): this { this.commands.push(["close"]); return this; }
  reset(): this { this.commands.length = 0; this.#minX = this.#minY = Infinity; this.#maxX = this.#maxY = -Infinity; return this; }
  addRect(x: number, y: number, w: number, h: number): this { this.commands.push(["addRect", x, y, w, h]); this.#pt(x, y); this.#pt(x + w, y + h); return this; }
  addOval(x: number, y: number, w: number, h: number): this { this.commands.push(["addOval", x, y, w, h]); this.#pt(x, y); this.#pt(x + w, y + h); return this; }
  addRRect(x: number, y: number, w: number, h: number, r: number): this { this.commands.push(["addRRect", x, y, w, h, r]); this.#pt(x, y); this.#pt(x + w, y + h); return this; }
}

export class Picture {
  constructor(readonly ops: RecordedOp[]) {}
  get opCount(): number { return this.ops.length; }
}

export class Image {
  #disposed = false;
  constructor(readonly width: number, readonly height: number) {}
  dispose(): void { this.#disposed = true; }
  get disposed(): boolean { return this.#disposed; }
}

export interface FrameInfo { image: Image; durationMs: number }

export class Codec {
  constructor(readonly frameCount: number, readonly repetitionCount: number, readonly image: Image) {}
  getNextFrame(): Promise<FrameInfo> { return Promise.resolve({ image: this.image, durationMs: 0 }); }
  dispose(): void {}
}

function toBytes(bytes: ArrayBuffer | ArrayBufferView): Uint8Array {
  return bytes instanceof ArrayBuffer ? new Uint8Array(bytes) : new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

/** mock 解码:前 8 字节为 uint32 LE 宽、高;否则 1x1。 */
function mockDecode(bytes: ArrayBuffer | ArrayBufferView): Image {
  const b = toBytes(bytes);
  control.decodedImages++;
  if (b.length >= 8) {
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    return new Image(dv.getUint32(0, true), dv.getUint32(4, true));
  }
  return new Image(1, 1);
}

export function decodeImage(bytes: ArrayBuffer | ArrayBufferView, targetWidth?: number, targetHeight?: number): Promise<Image> {
  const img = mockDecode(bytes);
  if (targetWidth !== undefined || targetHeight !== undefined) {
    return Promise.resolve(new Image(targetWidth ?? img.width, targetHeight ?? img.height));
  }
  return Promise.resolve(img);
}
export function instantiateCodec(bytes: ArrayBuffer | ArrayBufferView): Promise<Codec> {
  return Promise.resolve(new Codec(1, 0, mockDecode(bytes)));
}
export function imageFromPixels(pixels: Uint8Array, width: number, height: number): Image {
  return new Image(width, height);
}
export function loadAsset(path: string): Promise<ArrayBuffer> {
  const data = control.assets.get(path);
  if (!data) return Promise.reject(new Error(`asset not found: ${path}`));
  const copy = new Uint8Array(data.length);
  copy.set(data);
  return Promise.resolve(copy.buffer);
}

// ---------------------------------------------------------------------------
// 文本

export type TextAlign = "left" | "right" | "center" | "justify" | "start" | "end";
export type TextDirection = "ltr" | "rtl";
export type FontStyle = "normal" | "italic";
export type TextDecoration = "none" | "underline" | "overline" | "lineThrough";
export type TextDecorationStyle = "solid" | "double" | "dotted" | "dashed" | "wavy";
export type TextBaseline = "alphabetic" | "ideographic";

export interface TextStyle {
  color?: Color; fontFamilies?: string[]; fontSize?: number; fontWeight?: number; fontStyle?: FontStyle;
  letterSpacing?: number; wordSpacing?: number; height?: number; decoration?: TextDecoration;
  decorationColor?: Color; decorationStyle?: TextDecorationStyle; decorationThickness?: number;
  backgroundColor?: Color; textBaseline?: TextBaseline; locale?: string;
}
export interface ParagraphStyle {
  textAlign?: TextAlign; textDirection?: TextDirection; maxLines?: number; ellipsis?: string;
  textStyle?: TextStyle; height?: number;
}
export interface LineMetrics {
  startIndex: number; endIndex: number; hardBreak: boolean; ascent: number; descent: number;
  height: number; width: number; left: number; baseline: number; lineNumber: number;
}
export interface TextPosition { offset: number; affinity: "downstream" | "upstream" }

interface MockLine { start: number; end: number; hardBreak: boolean; width: number; height: number; top: number; left: number; fontSize: number }

const DEFAULT_FONT_SIZE = 14;

export class Paragraph {
  readonly text: string;
  readonly sizes: Float64Array; // 每个码元的字号
  readonly style: ParagraphStyle;
  readonly heightFactor: number;
  #lines: MockLine[] = [];
  #width = 0;
  #exceeded = false;
  #disposed = false;

  constructor(text: string, sizes: Float64Array, style: ParagraphStyle) {
    this.text = text;
    this.sizes = sizes;
    this.style = style;
    this.heightFactor = style.textStyle?.height ?? style.height ?? 1;
  }

  #charWidth(i: number): number {
    const c = this.text.charCodeAt(i);
    if (c === 10) return 0;
    return this.sizes[i] ?? DEFAULT_FONT_SIZE;
  }

  layout(width: number): void {
    control.paragraphLayouts++;
    this.#width = width;
    const lines: MockLine[] = [];
    const text = this.text;
    const n = text.length;
    let lineStart = 0;
    let top = 0;
    const maxLines = this.style.maxLines ?? Infinity;
    this.#exceeded = false;
    const pushLine = (start: number, end: number, hard: boolean): void => {
      let w = 0;
      let fs = 0;
      for (let i = start; i < end; i++) { w += this.#charWidth(i); fs = Math.max(fs, this.sizes[i] ?? DEFAULT_FONT_SIZE); }
      // 行尾空白不计宽度
      let j = end - 1;
      while (j >= start && (text[j] === " ")) { w -= this.#charWidth(j); j--; }
      if (fs === 0) fs = this.sizes[Math.min(start, n - 1)] ?? this.style.textStyle?.fontSize ?? DEFAULT_FONT_SIZE;
      const h = fs * this.heightFactor;
      lines.push({ start, end, hardBreak: hard, width: w, height: h, top, left: 0, fontSize: fs });
      top += h;
    };
    if (n === 0) {
      pushLine(0, 0, true);
    }
    while (lineStart < n) {
      let x = 0;
      let i = lineStart;
      let lastBreak = -1;
      let hard = false;
      for (; i < n; i++) {
        const c = text.charCodeAt(i);
        if (c === 10) { hard = true; break; }
        const cw = this.#charWidth(i);
        if (x + cw > width + 1e-9 && i > lineStart && c !== 32) {
          break;
        }
        x += cw;
        if (c === 32) lastBreak = i + 1;
      }
      if (hard) {
        pushLine(lineStart, i, true);
        lineStart = i + 1;
        if (lineStart === n) pushLine(n, n, true);
      } else if (i >= n) {
        pushLine(lineStart, n, true);
        lineStart = n;
      } else {
        const end = lastBreak > lineStart ? lastBreak : i;
        pushLine(lineStart, end, false);
        lineStart = end;
      }
      if (lines.length >= maxLines && lineStart < n) {
        this.#exceeded = true;
        break;
      }
    }
    const align = this.style.textAlign ?? "left";
    if (Number.isFinite(width)) {
      for (const l of lines) {
        if (align === "center") l.left = (width - l.width) / 2;
        else if (align === "right" || align === "end") l.left = width - l.width;
      }
    }
    this.#lines = lines;
  }

  get width(): number { return this.#width; }
  get height(): number { let h = 0; for (const l of this.#lines) h += l.height; return h; }
  get longestLine(): number { let w = 0; for (const l of this.#lines) w = Math.max(w, l.width); return w; }
  get minIntrinsicWidth(): number {
    let best = 0; let cur = 0;
    for (let i = 0; i < this.text.length; i++) {
      const c = this.text.charCodeAt(i);
      if (c === 32 || c === 10) { best = Math.max(best, cur); cur = 0; } else cur += this.#charWidth(i);
    }
    return Math.max(best, cur);
  }
  get maxIntrinsicWidth(): number {
    let best = 0; let cur = 0;
    for (let i = 0; i < this.text.length; i++) {
      if (this.text.charCodeAt(i) === 10) { best = Math.max(best, cur); cur = 0; } else cur += this.#charWidth(i);
    }
    // 去掉末尾空格
    let j = this.text.length - 1;
    while (j >= 0 && this.text[j] === " ") { cur -= this.#charWidth(j); j--; }
    return Math.max(best, cur);
  }
  get alphabeticBaseline(): number { const l = this.#lines[0]; return l ? l.fontSize * 0.8 * this.heightFactor : 0; }
  get ideographicBaseline(): number { const l = this.#lines[0]; return l ? l.fontSize * this.heightFactor : 0; }
  get didExceedMaxLines(): boolean { return this.#exceeded; }
  get lineCount(): number { return this.#lines.length; }

  #xOf(line: MockLine, index: number): number {
    let x = line.left;
    for (let i = line.start; i < index && i < line.end; i++) x += this.#charWidth(i);
    return x;
  }

  getBoxesForRange(start: number, end: number): Float64Array {
    const out: number[] = [];
    for (const l of this.#lines) {
      const s = Math.max(start, l.start);
      const e = Math.min(end, l.end);
      if (s >= e) continue;
      out.push(this.#xOf(l, s), l.top, this.#xOf(l, e), l.top + l.height, 1);
    }
    return new Float64Array(out);
  }

  getPositionForOffset(x: number, y: number): TextPosition {
    const lines = this.#lines;
    if (lines.length === 0) return { offset: 0, affinity: "downstream" };
    let line = lines[lines.length - 1]!;
    for (const l of lines) { if (y < l.top + l.height) { line = l; break; } }
    let px = line.left;
    let end = line.end;
    // 软折行的行尾空格不可定位到其后
    if (!line.hardBreak && end > line.start && this.text[end - 1] === " ") end--;
    for (let i = line.start; i < end; i++) {
      const w = this.#charWidth(i);
      if (x < px + w / 2) return { offset: i, affinity: "downstream" };
      px += w;
    }
    return { offset: end, affinity: line.hardBreak ? "downstream" : "upstream" };
  }

  getWordBoundary(offset: number): { start: number; end: number } {
    const t = this.text;
    const isWord = (c: string | undefined): boolean => c !== undefined && /[\p{L}\p{N}_]/u.test(c);
    if (!isWord(t[offset])) return { start: offset, end: Math.min(offset + 1, t.length) };
    let s = offset; while (s > 0 && isWord(t[s - 1])) s--;
    let e = offset; while (e < t.length && isWord(t[e])) e++;
    return { start: s, end: e };
  }

  getLineMetrics(): LineMetrics[] {
    return this.#lines.map((l, i) => ({
      startIndex: l.start, endIndex: l.end, hardBreak: l.hardBreak,
      ascent: l.fontSize * 0.8 * this.heightFactor, descent: l.fontSize * 0.2 * this.heightFactor,
      height: l.height, width: l.width, left: l.left, baseline: l.top + l.fontSize * 0.8 * this.heightFactor, lineNumber: i,
    }));
  }
  dispose(): void { this.#disposed = true; }
  get disposed(): boolean { return this.#disposed; }
}

export class ParagraphBuilder {
  #style: ParagraphStyle;
  #stack: number[] = [];
  #text = "";
  #sizes: number[] = [];
  constructor(style?: ParagraphStyle) {
    this.#style = style ?? {};
    this.#stack.push(this.#style.textStyle?.fontSize ?? DEFAULT_FONT_SIZE);
  }
  pushStyle(style: TextStyle): void { this.#stack.push(style.fontSize ?? this.#stack[this.#stack.length - 1]!); }
  pop(): void { if (this.#stack.length > 1) this.#stack.pop(); }
  addText(text: string): void {
    this.#text += text;
    const fs = this.#stack[this.#stack.length - 1]!;
    for (let i = 0; i < text.length; i++) this.#sizes.push(fs);
  }
  build(): Paragraph {
    control.paragraphsBuilt++;
    return new Paragraph(this.#text, new Float64Array(this.#sizes), this.#style);
  }
}

export function loadFontFromBytes(bytes: ArrayBuffer | ArrayBufferView, family?: string): string {
  return family ?? "MockFont";
}

// ---------------------------------------------------------------------------

export class Canvas {
  #ops: RecordedOp[];
  #saveCount = 1;
  #recording = true;
  constructor(ops: RecordedOp[]) { this.#ops = ops; }
  #rec(op: string, ...args: unknown[]): void {
    if (!this.#recording) throw new Error("Canvas used after endRecording");
    this.#ops.push({ op, args });
  }
  /** @internal */ finish(): void { this.#recording = false; }
  save(): void { this.#saveCount++; this.#rec("save"); }
  saveLayer(x: number | null, y: number | null, w: number | null, h: number | null, paint: Paint | null): void { this.#saveCount++; this.#rec("saveLayer", x, y, w, h, snapshotPaint(paint)); }
  restore(): void { if (this.#saveCount > 1) this.#saveCount--; this.#rec("restore"); }
  getSaveCount(): number { return this.#saveCount; }
  translate(dx: number, dy: number): void { this.#rec("translate", dx, dy); }
  scale(sx: number, sy?: number): void { this.#rec("scale", sx, sy ?? sx); }
  rotate(r: number): void { this.#rec("rotate", r); }
  transform(m: Matrix4): void { this.#rec("transform", Array.from(m)); }
  clipRect(x: number, y: number, w: number, h: number): void { this.#rec("clipRect", x, y, w, h); }
  clipRRect(x: number, y: number, w: number, h: number, r: number): void { this.#rec("clipRRect", x, y, w, h, r); }
  clipPath(path: Path): void { this.#rec("clipPath", path.commands.slice()); }
  drawColor(color: Color): void { this.#rec("drawColor", color); }
  drawPaint(paint: Paint): void { this.#rec("drawPaint", snapshotPaint(paint)); }
  drawRect(x: number, y: number, w: number, h: number, paint: Paint): void { this.#rec("drawRect", x, y, w, h, snapshotPaint(paint)); }
  drawRRect(x: number, y: number, w: number, h: number, r: number, paint: Paint): void { this.#rec("drawRRect", x, y, w, h, r, snapshotPaint(paint)); }
  drawDRRect(ox: number, oy: number, ow: number, oh: number, or: number, ix: number, iy: number, iw: number, ih: number, ir: number, paint: Paint): void {
    this.#rec("drawDRRect", ox, oy, ow, oh, or, ix, iy, iw, ih, ir, snapshotPaint(paint));
  }
  drawOval(x: number, y: number, w: number, h: number, paint: Paint): void { this.#rec("drawOval", x, y, w, h, snapshotPaint(paint)); }
  drawCircle(cx: number, cy: number, r: number, paint: Paint): void { this.#rec("drawCircle", cx, cy, r, snapshotPaint(paint)); }
  drawLine(x0: number, y0: number, x1: number, y1: number, paint: Paint): void { this.#rec("drawLine", x0, y0, x1, y1, snapshotPaint(paint)); }
  drawPath(path: Path, paint: Paint): void { this.#rec("drawPath", path.commands.slice(), snapshotPaint(paint)); }
  drawPicture(picture: Picture): void { this.#rec("drawPicture", picture); }
  drawImage(image: Image, x: number, y: number, paint?: Paint | null): void { this.#rec("drawImage", image, x, y); }
  drawImageRect(image: Image, sx: number, sy: number, sw: number, sh: number, dx: number, dy: number, dw: number, dh: number, paint?: Paint | null): void {
    this.#rec("drawImageRect", image, sx, sy, sw, sh, dx, dy, dw, dh);
  }
  drawParagraph(paragraph: Paragraph, x: number, y: number): void { this.#rec("drawParagraph", paragraph.text, x, y); }
  drawTexture(textureId: number, x: number, y: number, w: number, h: number): void { this.#rec("drawTexture", textureId, x, y, w, h); }
}

export class PictureRecorder {
  #canvas: Canvas | null = null;
  #ops: RecordedOp[] = [];
  get isRecording(): boolean { return this.#canvas !== null; }
  beginRecording(cullX?: number, cullY?: number, cullWidth?: number, cullHeight?: number): Canvas {
    if (this.#canvas) throw new Error("already recording");
    this.#ops = [];
    this.#canvas = new Canvas(this.#ops);
    return this.#canvas;
  }
  endRecording(): Picture {
    if (!this.#canvas) throw new Error("not recording");
    this.#canvas.finish();
    this.#canvas = null;
    return new Picture(this.#ops);
  }
}

export class Scene {
  constructor(readonly ops: RecordedOp[]) {}
}

export class SceneBuilder {
  #ops: RecordedOp[] = [];
  #depth = 0;
  pushOffset(dx: number, dy: number): this { this.#depth++; this.#ops.push({ op: "pushOffset", args: [dx, dy] }); return this; }
  pushTransform(m: Matrix4): this { this.#depth++; this.#ops.push({ op: "pushTransform", args: [Array.from(m)] }); return this; }
  pushClipRect(x: number, y: number, w: number, h: number): this { this.#depth++; this.#ops.push({ op: "pushClipRect", args: [x, y, w, h] }); return this; }
  pushClipRRect(x: number, y: number, w: number, h: number, r: number): this { this.#depth++; this.#ops.push({ op: "pushClipRRect", args: [x, y, w, h, r] }); return this; }
  pushOpacity(alpha: number): this { this.#depth++; this.#ops.push({ op: "pushOpacity", args: [alpha] }); return this; }
  pop(): this { if (this.#depth <= 0) throw new Error("SceneBuilder.pop underflow"); this.#depth--; this.#ops.push({ op: "pop", args: [] }); return this; }
  addPicture(dx: number, dy: number, picture: Picture): this { this.#ops.push({ op: "addPicture", args: [dx, dy, picture] }); return this; }
  addTexture(id: number, x: number, y: number, w: number, h: number): this { this.#ops.push({ op: "addTexture", args: [id, x, y, w, h] }); return this; }
  addPlatformView(id: number, x: number, y: number, w: number, h: number): this { this.#ops.push({ op: "addPlatformView", args: [id, x, y, w, h] }); return this; }
  build(): Scene {
    while (this.#depth > 0) this.pop();
    const s = new Scene(this.#ops);
    this.#ops = [];
    return s;
  }
}

// ---------------------------------------------------------------------------

export const POINTER_DATA_STRIDE = 10;

export interface KeyData {
  type: "down" | "up" | "repeat"; timeStampUs: number; physicalKey: number; logicalKey: number; character: string; modifiers: number;
}

export type SystemCursor = "basic" | "text" | "click" | "precise" | "resizeLeftRight" | "resizeUpDown" | "forbidden" | "none";

interface MockView {
  readonly id: number;
  devicePixelRatio: number;
  physicalSize: { width: number; height: number };
  focused: boolean;
  title: string;
  onMetricsChanged: (() => void) | null;
  onCloseRequested: (() => boolean | void) | null;
  onPointerPacket: ((data: Float64Array, count: number) => void) | null;
  onKeyData: ((data: KeyData) => boolean | void) | null;
  onFocusChanged: ((focused: boolean) => void) | null;
  render(scene: Scene): void;
  setCursor(cursor: SystemCursor): void;
  setMinSize(width: number, height: number): void;
  close(): void;
  setAlwaysOnTop(on: boolean): void;
  activate(): void;
  setTitleBarStyle(style: "default" | "hidden", center?: number): void;
  setDragRegions(rects: readonly number[]): void;
}

interface MockWindow extends MockView {
  onBeginFrame: ((frameTimeMs: number) => void) | null;
  onDrawFrame: (() => void) | null;
  onViewsChanged: (() => void) | null;
  scheduleFrame(): void;
}

const viewMap = new Map<number, MockView>();
let nextViewId = 1;

function makeView(id: number, width: number, height: number, dpr: number): MockView {
  return {
    id,
    devicePixelRatio: dpr,
    physicalSize: { width, height },
    focused: id === 0,
    get title(): string { return control.titles.get(id) ?? ""; },
    set title(v: string) {
      control.titles.set(id, v);
      if (id === 0) control.title = v;
    },
    onMetricsChanged: null,
    onCloseRequested: null,
    onPointerPacket: null,
    onKeyData: null,
    onFocusChanged: null,
    render(scene: Scene): void {
      if (!viewMap.has(id)) return;
      control.sceneOpsByView.set(id, scene.ops);
      if (id === 0) {
        control.renderedScenes++;
        control.lastSceneOps = scene.ops;
      }
    },
    setCursor(cursor: SystemCursor): void { control.cursor = cursor; },
    setMinSize(): void {},
    setAlwaysOnTop(on: boolean): void { control.alwaysOnTop.set(id, on); },
    setTitleBarStyle(style: "default" | "hidden", center?: number): void { control.titleBars.set(id, { style, center: center ?? null }); },
    setDragRegions(rects: readonly number[]): void { control.dragRegions.set(id, [...rects]); },
    activate(): void { control.activated.push(id); },
    close(): void {
      if (id === 0) throw new Error("view.close: the main window (view 0) cannot be closed from JS");
      removeView(id);
    },
  };
}

function removeView(id: number): void {
  if (!viewMap.delete(id)) return;
  control.sceneOpsByView.delete(id);
  // 宿主异步通知(真实引擎经 ui 线程任务): 用 microtask 模拟。
  void Promise.resolve().then(() => win.onViewsChanged?.());
}

const win: MockWindow = Object.assign(makeView(0, 800, 600, 1), {
  get title(): string { return control.title; },
  set title(v: string) { control.title = v; control.titles.set(0, v); },
  onBeginFrame: null,
  onDrawFrame: null,
  onViewsChanged: null,
  scheduleFrame(): void { control.frameRequests++; },
}) as MockWindow;
// Object.assign 不复制访问器: 重新定义 title。
Object.defineProperty(win, "title", {
  get: () => control.title,
  set: (v: string) => { control.title = v; control.titles.set(0, v); },
});
viewMap.set(0, win);

export const window = win;
export const views: ReadonlyMap<number, MockView> = viewMap;

/** 宿主异步创建窗口: 先加入 views 并通知 onViewsChanged, 再解析 Promise(与真实引擎顺序一致)。 */
export function createView(o: { title?: string; width: number; height: number }): Promise<MockView> {
  return new Promise((resolve, reject) => {
    void Promise.resolve().then(() => {
      if (control.failNextCreateView) {
        control.failNextCreateView = false;
        reject(new Error("createView: the host refused to open a window"));
        return;
      }
      const id = nextViewId++;
      const v = makeView(id, o.width * win.devicePixelRatio, o.height * win.devicePixelRatio, win.devicePixelRatio);
      if (o.title) v.title = o.title;
      viewMap.set(id, v);
      win.onViewsChanged?.();
      resolve(v);
    });
  });
}
