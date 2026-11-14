// momo:ui 的纯 TS mock(Node 宿主下替代引擎原生模块)。
// - Canvas/SceneBuilder 记录调用;
// - window 可模拟 vsync(由 ViewTester 直接调用 onBeginFrame/onDrawFrame);
// - Paragraph 采用 Ahem 固定宽度字形模型:每个 UTF-16 码元宽 = 字号, 行高 = 字号 × height(默认 1),
//   ascent = 0.8 × 字号, descent = 0.2 × 字号;按空格/换行贪心折行。
import type { MockControl, RecordedOp } from "../host";
import { MethodCall, PlatformException } from "../../packages/momo/src/services/message_codecs";
import { StandardMethodCodec } from "../../packages/momo/src/services/standard_codec";
import { __mockServices } from "./momo_services";

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
    const v = viewMap.get(0);
    if (v === undefined) return;
    v.physicalSize = { width, height };
    v.devicePixelRatio = dpr;
    const f = control.frames.get(0);
    if (f !== undefined) control.frames.set(0, { ...f, width: width / dpr, height: height / dpr });
  },
  pictureOps(picture: unknown): RecordedOp[] {
    return picture instanceof Picture ? picture.ops : [];
  },
  sceneOpsByView: new Map<number, RecordedOp[]>(),
  titles: new Map<number, string>(),
  openViews(): number[] {
    return [...viewMap.keys()].filter((id) => id !== 0);
  },
  hasView(id: number): boolean {
    return viewMap.has(id);
  },
  requestCloseView(id: number): Promise<boolean> {
    return hostRequestClose(id);
  },
  requestQuit(): Promise<boolean> {
    return hostQuit();
  },
  exitCode: null,
  appCalls: [],
  setWindowState(id, state): void {
    setState(id, state);
  },
  windowStates: new Map(),
  setWindowFocus(id, focused): void {
    const v = viewMap.get(id);
    if (v === undefined) return;
    v.focused = focused;
    v.onFocusChanged?.(focused);
    notify("focusChanged", { view: id, focused });
  },
  reopen(hasVisibleWindows: boolean): void {
    notifyApp("activate", { hasVisibleWindows });
  },
  failNextCreateView: false,
  alwaysOnTop: new Map<number, boolean>(),
  activated: [],
  captures: [],
  panelViews: new Set<number>(),
  titleBars: new Map<number, { hidden: boolean; height: number }>(),
  resizable: new Map<number, boolean>(),
  resizeBorders: new Map<number, { left?: number; top?: number; right?: number; bottom?: number }>(),
  lastCreateViewOptions: null,
  frames: new Map<number, { x: number; y: number; width: number; height: number }>(),
  windowDrags: [],
  moveLoops: new Map<number, { x: number; y: number }>(),
  minSizes: new Map<number, { width: number; height: number }>(),
  deliverPointer(view: number, data: Float64Array, count: number): void {
    hostPointer(view, data, count);
  },
  debugRestoreImplicitView(): void {
    captureView = null;
    activeSequence = 0;
    control.exitCode = null;
    control.appCalls = [];
    quitting = false;
    // 测试之间不留窗口: 静默移除其他视图(框架侧的 Widget 对象由 Widget.debugReset 作废)。
    for (const id of [...viewMap.keys()]) {
      if (id === 0) continue;
      viewMap.delete(id);
      control.sceneOpsByView.delete(id);
      control.frames.delete(id);
      control.windowStates.delete(id);
      firstFrame.delete(id);
    }
    const startup = viewMap.get(0);
    if (startup !== undefined) {
      startup.visible = true;
      control.windowStates.set(0, { ...kShown });
      return;
    }
    viewMap.set(0, makeView(0, 800, 600, 1));
    control.frames.set(0, { x: 0, y: 0, width: 800, height: 600 });
    control.windowStates.set(0, { ...kShown });
    win.onViewportsChanged?.();
  },
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
  addHitRegion(x: number, y: number, w: number, h: number): void { this.#rec("addHitRegion", x, y, w, h); }
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

export const POINTER_DATA_STRIDE = 17;

export interface KeyData {
  type: "down" | "up" | "repeat"; timeStampUs: number; physicalKey: number; logicalKey: number; character: string; modifiers: number;
}

export type SystemCursor = "basic" | "text" | "click" | "precise" | "resizeLeftRight" | "resizeUpDown" | "forbidden" | "none";

/** momo:ui RootView 的 mock: 只有引擎职责(画面、尺寸、输入、捕获)。窗口管理由下方的宿主 mock(momo/window)提供。 */
interface MockView {
  readonly id: number;
  devicePixelRatio: number;
  physicalSize: { width: number; height: number };
  focused: boolean;
  visible: boolean;
  onMetricsChanged: (() => void) | null;
  onPointerPacket: ((data: Float64Array, count: number) => void) | null;
  onKeyData: ((data: KeyData) => boolean | void) | null;
  onFocusChanged: ((focused: boolean) => void) | null;
  onVisibilityChanged: ((visible: boolean) => void) | null;
  render(scene: Scene): void;
}

interface MockWindow {
  onBeginFrame: ((frameTimeMs: number) => void) | null;
  onDrawFrame: (() => void) | null;
  onViewportsChanged: (() => void) | null;
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
    visible: true,
    onMetricsChanged: null,
    onPointerPacket: null,
    onKeyData: null,
    onFocusChanged: null,
    onVisibilityChanged: null,
    render(scene: Scene): void {
      if (!viewMap.has(id)) return;
      // 引擎: 不可见的视图提交的画面被丢弃。
      if (!this.visible) return;
      control.sceneOpsByView.set(id, scene.ops);
      if (!firstFrame.has(id)) {
        firstFrame.add(id);
        notify("readyToShow", { view: id });
      }
      if (id === 0) {
        control.renderedScenes++;
        control.lastSceneOps = scene.ops;
      }
    },
  };
}

function removeView(id: number): void {
  if (!viewMap.has(id)) return;
  // 宿主: 关闭的窗口是进行中序列的接收窗口时, 以取消结束序列(按下中 remove)。
  if (captureView === id) {
    captureView = null;
    const d = new Float64Array(POINTER_DATA_STRIDE);
    d[2] = 4;  // remove
    viewMap.get(id)?.onPointerPacket?.(d, 1);
  }
  viewMap.delete(id);
  control.sceneOpsByView.delete(id);
  control.frames.delete(id);
  control.moveLoops.delete(id);
  control.windowStates.delete(id);
  firstFrame.delete(id);
  const last = viewMap.size === 0;
  // 宿主异步通知(真实引擎经 ui 线程任务): 用 microtask 模拟。视图先移除, 再发 closed;最后一个窗口关闭时
  // (不在退出流程中)发 windowAllClosed, 应用继续运行。
  void Promise.resolve().then(() => {
    win.onViewportsChanged?.();
    notify("closed", { view: id });
    if (last && !quitting && viewMap.size === 0) notifyApp("windowAllClosed", {});
  });
}

const kShown = { visible: true, minimized: false, maximized: false, fullscreen: false };
/** 已呈现过画面的视图(宿主据此发 readyToShow)。 */
const firstFrame = new Set<number>();

/** 宿主改变窗口状态: 上报 stateChanged;可见性(显示且未最小化)同步给视图。 */
function setState(view: number, change: Partial<typeof kShown>): void {
  const old = control.windowStates.get(view);
  const v = viewMap.get(view);
  if (old === undefined || v === undefined) return;
  const next = { ...old, ...change };
  if (JSON.stringify(next) === JSON.stringify(old)) return;
  control.windowStates.set(view, next);
  notify("stateChanged", { view, state: next });
  const visible = next.visible && !next.minimized;
  if (visible !== v.visible) {
    v.visible = visible;
    v.onVisibilityChanged?.(visible);
  }
}

const win: MockWindow = {
  onBeginFrame: null,
  onDrawFrame: null,
  onViewportsChanged: null,
  scheduleFrame(): void { control.frameRequests++; },
};
viewMap.set(0, makeView(0, 800, 600, 1));
control.frames.set(0, { x: 0, y: 0, width: 800, height: 600 });
control.windowStates.set(0, { ...kShown });

export const platformDispatcher = win;
export const viewports: ReadonlyMap<number, MockView> = viewMap;

// ---------------------------------------------------------------------------
// 宿主 mock: momo/window(真实实现见 embedder/common/window_channel.cc)。处理器同步执行(状态在调用时即更新),
// 宿主 → JS 的通知(frameChanged / controlsChanged)经 momo:services mock 投递。

const codec = StandardMethodCodec.instance;

function notify(method: string, args: Record<string, unknown>): void {
  void __mockServices.deliver("momo/window", codec.encodeMethodCall(new MethodCall(method, args)));
}

function notifyApp(method: string, args: Record<string, unknown>): void {
  void __mockServices.deliver("momo/app", codec.encodeMethodCall(new MethodCall(method, args)));
}

/** 宿主 → JS 的询问(回复 bool;无处理器或出错视为允许)。 */
async function ask(channel: string, method: string, args: Record<string, unknown>): Promise<boolean> {
  const reply = await __mockServices.deliver(channel, codec.encodeMethodCall(new MethodCall(method, args)));
  if (reply === null) return true;
  try {
    return codec.decodeEnvelope(reply) !== false;
  } catch {
    return true;
  }
}

/** 用户点了关闭按钮(或 requestClose): 询问 JS, 允许则关闭。 */
async function hostRequestClose(view: number): Promise<boolean> {
  if (!viewMap.has(view)) return false;
  const allow = await ask("momo/window", "closeRequested", { view });
  if (allow) removeView(view);
  return allow;
}

let quitting = false;
/** 退出流程(同 embedder/common/application.cc 的 Quit)。 */
async function hostQuit(): Promise<boolean> {
  if (quitting || control.exitCode !== null) return false;
  quitting = true;
  try {
    if (!(await ask("momo/app", "beforeQuit", {}))) return false;
    const results = await Promise.all([...viewMap.keys()].map((id) => hostRequestClose(id)));
    if (results.includes(false)) return false;
    if (!(await ask("momo/app", "willQuit", {}))) return false;
    hostExit(0);
    return true;
  } finally {
    quitting = false;
  }
}

function hostExit(code: number): void {
  control.exitCode = code;
  notifyApp("stateChanged", { state: "terminating" });
}

function setFrame(view: number, frame: { x: number; y: number; width: number; height: number }): void {
  control.frames.set(view, frame);
  notify("frameChanged", { view, frame: [frame.x, frame.y, frame.width, frame.height] });
}

function controlsOf(view: number): number[] | null {
  const bar = control.titleBars.get(view);
  return bar?.hidden ? [0, 0, 78, bar.height > 0 ? bar.height : 28] : null;
}

/** 进行中序列的原生接收窗口(momo/window capturePointer 改交;抬起后结束)。 */
let captureView: number | null = null;
/** 进行中的按下序列 id(同真实宿主: 首个按下分配, 最后一个抬起后清零)。 */
let activeSequence = 0;
let nextSequence = 1;

/**
 * 宿主收到的指针事件: 序列改交过接收窗口时改投它(坐标按屏幕位置换算到它的内容区, 同 macOS 宿主);
 * 移动循环先移动窗口(与 macOS 宿主一致), 再交给视图。
 */
function hostPointer(view: number, data: Float64Array, count: number): void {
  if (captureView !== null && captureView !== view) {
    const target = captureView;
    const f = control.frames.get(target);
    const dpr = viewMap.get(target)?.devicePixelRatio ?? 1;
    for (let i = 0; i < count; i++) {
      const o = i * POINTER_DATA_STRIDE;
      if (data[o + 2] === 0 /* add */ || data[o + 2] === 5 /* hover */) continue;
      if (f !== undefined) {
        data[o + 4] = (data[o + 10]! - f.x) * dpr;
        data[o + 5] = (data[o + 11]! - f.y) * dpr;
      }
    }
    view = target;
  }
  for (let i = 0; i < count; i++) {
    const o = i * POINTER_DATA_STRIDE;
    const phase = data[o + 2];
    if (phase === 1 /* down */ && activeSequence === 0) activeSequence = nextSequence++;
    if (activeSequence !== 0 && (phase === 1 || phase === 2 || phase === 3 || phase === 7 || phase === 8)) data[o + 12] = activeSequence;
    if (phase === 3 /* up */ || phase === 8 /* cancel */) {
      captureView = null;
      activeSequence = 0;
    }
  }
  for (let i = 0; i < count; i++) {
    const o = i * POINTER_DATA_STRIDE;
    const loop = control.moveLoops.get(view);
    if (loop === undefined) continue;
    const phase = data[o + 2];
    if (phase === 2 /* move */) {
      const f = control.frames.get(view)!;
      setFrame(view, { ...f, x: data[o + 10]! - loop.x, y: data[o + 11]! - loop.y });
    } else if (phase === 3 /* up */) {
      control.moveLoops.delete(view);
    }
  }
  viewMap.get(view)?.onPointerPacket?.(data, count);
}

function handleWindowCall(call: MethodCall): unknown {
  const a = (call.args ?? {}) as Record<string, unknown>;
  if (call.method === "create") {
    control.lastCreateViewOptions = { ...(a as { width: number; height: number }) };
    if (control.failNextCreateView) {
      control.failNextCreateView = false;
      throw new PlatformException("error", "the host refused to open a window");
    }
    const id = nextViewId++;
    const dpr = viewMap.get(0)?.devicePixelRatio ?? 1;
    const width = Number(a["width"]);
    const height = Number(a["height"]);
    viewMap.set(id, makeView(id, width * dpr, height * dpr, dpr));
    control.frames.set(id, { x: typeof a["x"] === "number" ? a["x"] : 200, y: typeof a["y"] === "number" ? a["y"] : 120, width, height });
    if (typeof a["title"] === "string" && a["title"] !== "") control.titles.set(id, a["title"]);
    if (a["nonActivating"] === true) control.panelViews.add(id);
    if (a["resizable"] === false) control.resizable.set(id, false);
    control.windowStates.set(id, { ...kShown, visible: a["show"] !== false });
    win.onViewportsChanged?.();  // 视图先加入(真实宿主: AddView 先于回复到达 ui 线程)
    return id;
  }
  const view = Number(a["view"]);
  if (call.method === "close") {
    removeView(view);
    return null;
  }
  if (call.method === "requestClose") {
    void hostRequestClose(view);
    return null;
  }
  if (!viewMap.has(view)) throw new PlatformException("notFound", `no window for view ${view}`);
  switch (call.method) {
    case "getState": {
      const f = control.frames.get(view)!;
      return {
        frame: [f.x, f.y, f.width, f.height], controls: controlsOf(view), state: control.windowStates.get(view),
        capabilities: ["alwaysOnTop", "position", "resizeOptions", "hiddenTitleBar", "windowDrag", "moveLoop", "pointerCapture",
                       "platformViews", "nonActivating", "visibility", "minMax"],
      };
    }
    case "show":
      setState(view, { visible: true, minimized: false });
      return null;
    case "hide":
      setState(view, { visible: false });
      return null;
    case "minimize":
      setState(view, { minimized: true });
      return null;
    case "maximize":
      setState(view, { maximized: true, minimized: false });
      return null;
    case "restore": {
      const st = control.windowStates.get(view)!;
      setState(view, st.minimized ? { minimized: false } : { maximized: false });
      return null;
    }
    case "setTitle":
      control.titles.set(view, String(a["title"]));
      if (view === 0) control.title = String(a["title"]);
      return null;
    case "setMinSize":
      control.minSizes.set(view, { width: Number(a["width"]), height: Number(a["height"]) });
      return null;
    case "setCursor":
      control.cursor = String(a["cursor"]);
      return null;
    case "setAlwaysOnTop":
      control.alwaysOnTop.set(view, a["on"] === true);
      return null;
    case "activate":
      control.activated.push(view);
      return null;
    case "setPosition": {
      const f = control.frames.get(view)!;
      setFrame(view, { ...f, x: Number(a["x"]), y: Number(a["y"]) });
      return null;
    }
    case "setResizable":
      control.resizable.set(view, a["resizable"] === true);
      return null;
    case "setResizeBorder": {
      const { view: _v, reset, ...edges } = a;
      // reset: 各边恢复宿主默认值(不再记录覆盖值)。
      const base = reset === true ? {} : control.resizeBorders.get(view);
      if (reset === true && Object.keys(edges).length === 0) control.resizeBorders.delete(view);
      else control.resizeBorders.set(view, { ...base, ...(edges as Record<string, number>) });
      return null;
    }
    case "setTitleBar":
      control.titleBars.set(view, { hidden: a["hidden"] === true, height: Number(a["height"] ?? 0) });
      notify("controlsChanged", { view, controls: controlsOf(view) });
      return null;
    case "startDrag":
      control.windowDrags.push({ view, sequenceId: Number(a["sequenceId"]) });
      return null;
    case "startMoveLoop":
      control.moveLoops.set(view, { x: Number(a["x"]), y: Number(a["y"]) });
      return null;
    case "endMoveLoop":
      control.moveLoops.delete(view);
      return null;
    case "capturePointer":
      if (activeSequence === 0 || Number(a["sequenceId"]) !== activeSequence) return false;
      control.captures.push(view);
      captureView = view;
      return true;
    default:
      throw new PlatformException("unimplemented", `momo/window: unknown method ${call.method}`);
  }
}

__mockServices.setNativeHandler("momo/window", (data) => {
  let call: MethodCall;
  try {
    call = codec.decodeMethodCall(data);
  } catch (e) {
    return codec.encodeErrorEnvelope("error", String(e));
  }
  try {
    return codec.encodeSuccessEnvelope(handleWindowCall(call) ?? null);
  } catch (e) {
    if (e instanceof PlatformException) return codec.encodeErrorEnvelope(e.code, e.platformMessage);
    return codec.encodeErrorEnvelope("error", String(e));
  }
});

// ---------------------------------------------------------------------------
// 宿主 mock: momo/app(真实实现见 embedder/common/app_channel.cc)。

function handleAppCall(call: MethodCall): unknown {
  const a = (call.args ?? {}) as Record<string, unknown>;
  control.appCalls.push(call.method);
  switch (call.method) {
    case "quit":
      void hostQuit();
      return null;
    case "exit":
      hostExit(typeof a["code"] === "number" ? a["code"] : 0);
      return null;
    case "hide":
    case "show":
    case "focus":
      return null;
    case "getState":
      return { state: control.exitCode !== null ? "terminating" : "running", active: true, capabilities: ["hide", "activate"] };
    default:
      throw new PlatformException("unimplemented", `momo/app: unknown method ${call.method}`);
  }
}

__mockServices.setNativeHandler("momo/app", (data) => {
  try {
    return codec.encodeSuccessEnvelope(handleAppCall(codec.decodeMethodCall(data)) ?? null);
  } catch (e) {
    if (e instanceof PlatformException) return codec.encodeErrorEnvelope(e.code, e.platformMessage);
    return codec.encodeErrorEnvelope("error", String(e));
  }
});
