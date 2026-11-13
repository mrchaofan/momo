// momo:ui / momo:services —— 引擎原生模块的 TypeScript 声明(docs/02 §4、docs/04 §5-6)。
// **这是 JS↔C++ 的接口契约文件**: 修改须同步 engine/ui/*_bindings.cc。
// 对应 C++ 实现: engine/ui/{painting,scene,window,text,image,services}_bindings.cc。
// 约定:
//   - 颜色为 0xAARRGGBB 数字(非预乘)。
//   - Canvas 坐标单位由调用方决定;Scene 根坐标系为物理像素(框架层按 devicePixelRatio 缩放)。
//   - 矩阵为 16 个数, 列主序(与 Flutter Matrix4 / WebGL 一致)。
//   - 文本下标一律为 UTF-16 码元下标(与 JS 字符串一致)。
//   - 参数类型错误抛 TypeError;状态错误(如录制结束后继续画)抛 Error。

declare module "momo:ui" {
  export type Color = number;
  export type Matrix4 = ArrayLike<number>;
  export type PaintingStyle = "fill" | "stroke";
  export type StrokeCap = "butt" | "round" | "square";
  export type StrokeJoin = "miter" | "round" | "bevel";
  export type TileMode = "clamp" | "repeat" | "mirror";
  export type FillType = "nonzero" | "evenodd";
  export type FilterQuality = "none" | "low" | "medium" | "high";

  export class Gradient {
    private constructor();
    static linear(x0: number, y0: number, x1: number, y1: number, colors: Color[],
                  stops?: number[] | null, tileMode?: TileMode): Gradient;
    static radial(cx: number, cy: number, radius: number, colors: Color[],
                  stops?: number[] | null, tileMode?: TileMode): Gradient;
  }

  export class Paint {
    color: Color;
    style: PaintingStyle;
    strokeWidth: number;
    strokeCap: StrokeCap;
    strokeJoin: StrokeJoin;
    antiAlias: boolean;
    shader: Gradient | null;
    /** drawImage* 的采样质量, 默认 "low"(双线性)。 */
    filterQuality: FilterQuality;
    /** 高斯模糊遮罩 sigma(像素), 0 = 无。用于阴影。 */
    maskBlurSigma: number;
  }

  export class Path {
    fillType: FillType;
    readonly bounds: { x: number; y: number; width: number; height: number };
    moveTo(x: number, y: number): this;
    lineTo(x: number, y: number): this;
    quadTo(cx: number, cy: number, x: number, y: number): this;
    cubicTo(c1x: number, c1y: number, c2x: number, c2y: number, x: number, y: number): this;
    close(): this;
    reset(): this;
    addRect(x: number, y: number, width: number, height: number): this;
    addOval(x: number, y: number, width: number, height: number): this;
    addRRect(x: number, y: number, width: number, height: number, radius: number): this;
  }

  export class Picture {
    private constructor();
    readonly opCount: number;
  }

  // ---------------------------------------------------------------------------
  // 图片(M3)。解码在 io 线程进行, 结果经 Promise 回到 ui 线程。

  export class Image {
    private constructor();
    readonly width: number;
    readonly height: number;
    /** 释放像素内存(之后再绘制该 Image 为空操作)。重复调用无害。 */
    dispose(): void;
    readonly disposed: boolean;
  }

  export interface FrameInfo {
    image: Image;
    /** 本帧显示时长(毫秒);静态图为 0。 */
    durationMs: number;
  }

  /** 多帧图片(GIF/WebP 动画)解码器。 */
  export class Codec {
    private constructor();
    readonly frameCount: number;
    /** -1 = 无限循环。 */
    readonly repetitionCount: number;
    /** 依次解码下一帧(到末尾后回到第 0 帧)。 */
    getNextFrame(): Promise<FrameInfo>;
    dispose(): void;
  }

  /** 解码静态图片(PNG/JPEG/WebP/GIF 首帧/BMP/ICO)。可选目标尺寸(像素)缩放。 */
  export function decodeImage(bytes: ArrayBuffer | ArrayBufferView,
                              targetWidth?: number, targetHeight?: number): Promise<Image>;
  export function instantiateCodec(bytes: ArrayBuffer | ArrayBufferView): Promise<Codec>;
  /** 由 RGBA8888(非预乘)像素直接构造图片(同步)。 */
  export function imageFromPixels(pixels: Uint8Array, width: number, height: number): Image;
  /** 读取应用资源目录(assets/)下的文件(io 线程)。路径不得逃出资源目录。 */
  export function loadAsset(path: string): Promise<ArrayBuffer>;

  // ---------------------------------------------------------------------------
  // 文本(M3): SkParagraph 封装。

  export type TextAlign = "left" | "right" | "center" | "justify" | "start" | "end";
  export type TextDirection = "ltr" | "rtl";
  export type FontStyle = "normal" | "italic";
  export type TextDecoration = "none" | "underline" | "overline" | "lineThrough";
  export type TextDecorationStyle = "solid" | "double" | "dotted" | "dashed" | "wavy";
  export type TextBaseline = "alphabetic" | "ideographic";

  export interface TextStyle {
    color?: Color;
    /** 字体族回退链, 依次尝试;为空用系统默认。 */
    fontFamilies?: string[];
    fontSize?: number;
    /** 100..900 */
    fontWeight?: number;
    fontStyle?: FontStyle;
    letterSpacing?: number;
    wordSpacing?: number;
    /** 行高倍数(相对字号);未设置 = 字体自身度量。 */
    height?: number;
    decoration?: TextDecoration;
    decorationColor?: Color;
    decorationStyle?: TextDecorationStyle;
    decorationThickness?: number;
    /** 文字背景色。 */
    backgroundColor?: Color;
    textBaseline?: TextBaseline;
    locale?: string;
  }

  export interface ParagraphStyle {
    textAlign?: TextAlign;
    textDirection?: TextDirection;
    maxLines?: number;
    /** 超出 maxLines 时末尾显示的省略串(如 "…")。 */
    ellipsis?: string;
    /** 段落默认文本样式(pushStyle 在其上叠加)。 */
    textStyle?: TextStyle;
    /** 行高倍数(段落级 strut)。 */
    height?: number;
  }

  export interface LineMetrics {
    startIndex: number;
    endIndex: number;
    hardBreak: boolean;
    ascent: number;
    descent: number;
    height: number;
    width: number;
    left: number;
    baseline: number;
    lineNumber: number;
  }

  export interface TextPosition {
    offset: number;
    /** "downstream" | "upstream" */
    affinity: "downstream" | "upstream";
  }

  export class Paragraph {
    private constructor();
    /** 以给定最大宽度排版(可重复调用);width 可为 Infinity(不限宽, 测固有宽度)。 */
    layout(width: number): void;
    readonly width: number;
    readonly height: number;
    readonly longestLine: number;
    readonly minIntrinsicWidth: number;
    readonly maxIntrinsicWidth: number;
    readonly alphabeticBaseline: number;
    readonly ideographicBaseline: number;
    readonly didExceedMaxLines: boolean;
    readonly lineCount: number;
    /**
     * [start, end) 范围的选区矩形, 平铺为 Float64Array: 每个矩形 5 个数 (left, top, right, bottom, dir),
     * dir: 0 = rtl, 1 = ltr。
     */
    getBoxesForRange(start: number, end: number): Float64Array;
    getPositionForOffset(x: number, y: number): TextPosition;
    /** 返回包含 offset 的单词边界 [start, end)。 */
    getWordBoundary(offset: number): { start: number; end: number };
    getLineMetrics(): LineMetrics[];
    /** 释放原生对象(可选, 否则随 GC)。 */
    dispose(): void;
  }

  export class ParagraphBuilder {
    constructor(style?: ParagraphStyle);
    pushStyle(style: TextStyle): void;
    pop(): void;
    addText(text: string): void;
    /** 构建后 builder 失效。 */
    build(): Paragraph;
  }

  /** 注册应用内嵌字体(TTF/OTF/TTC 字节)。family 缺省时取字体自身的族名。返回实际族名。 */
  export function loadFontFromBytes(bytes: ArrayBuffer | ArrayBufferView, family?: string): string;

  // ---------------------------------------------------------------------------

  export class Canvas {
    private constructor();
    save(): void;
    /** 离屏图层(用于整体透明度等)。bounds 可省略。 */
    saveLayer(x: number | null, y: number | null, width: number | null, height: number | null,
              paint: Paint | null): void;
    restore(): void;
    getSaveCount(): number;
    translate(dx: number, dy: number): void;
    scale(sx: number, sy?: number): void;
    rotate(radians: number): void;
    transform(matrix: Matrix4): void;
    clipRect(x: number, y: number, width: number, height: number): void;
    clipRRect(x: number, y: number, width: number, height: number, radius: number): void;
    clipPath(path: Path): void;
    /** 记录可命中区域(局部坐标, 不绘制): 宿主据此判断这块 momo 内容是否盖住下面的平台视图。 */
    addHitRegion(x: number, y: number, width: number, height: number): void;
    drawColor(color: Color): void;
    drawPaint(paint: Paint): void;
    drawRect(x: number, y: number, width: number, height: number, paint: Paint): void;
    drawRRect(x: number, y: number, width: number, height: number, radius: number, paint: Paint): void;
    /** 内外两个圆角矩形之间的区域(边框)。 */
    drawDRRect(ox: number, oy: number, ow: number, oh: number, oradius: number,
               ix: number, iy: number, iw: number, ih: number, iradius: number, paint: Paint): void;
    drawOval(x: number, y: number, width: number, height: number, paint: Paint): void;
    drawCircle(cx: number, cy: number, radius: number, paint: Paint): void;
    drawLine(x0: number, y0: number, x1: number, y1: number, paint: Paint): void;
    drawPath(path: Path, paint: Paint): void;
    drawPicture(picture: Picture): void;
    drawImage(image: Image, x: number, y: number, paint?: Paint | null): void;
    drawImageRect(image: Image, sx: number, sy: number, sw: number, sh: number,
                  dx: number, dy: number, dw: number, dh: number, paint?: Paint | null): void;
    drawParagraph(paragraph: Paragraph, x: number, y: number): void;
    /** 外部纹理(扩展注入, docs/07 §3)占位绘制。 */
    drawTexture(textureId: number, x: number, y: number, width: number, height: number): void;
  }

  export class PictureRecorder {
    readonly isRecording: boolean;
    /** 可选 cull rect 用于约束 drawColor/drawPaint 等无界绘制的包围盒。 */
    beginRecording(cullX?: number, cullY?: number, cullWidth?: number, cullHeight?: number): Canvas;
    endRecording(): Picture;
  }

  export class Scene {
    private constructor();
  }

  export class SceneBuilder {
    pushOffset(dx: number, dy: number): this;
    pushTransform(matrix: Matrix4): this;
    pushClipRect(x: number, y: number, width: number, height: number): this;
    pushClipRRect(x: number, y: number, width: number, height: number, radius: number): this;
    /** alpha: 0..255 */
    pushOpacity(alpha: number): this;
    pop(): this;
    addPicture(dx: number, dy: number, picture: Picture): this;
    /** 外部纹理层(扩展注入)。 */
    addTexture(textureId: number, x: number, y: number, width: number, height: number): this;
    /**
     * 平台视图占位层(docs/05 §9): 由扩展创建的系统原生视图(如 WKWebView)在此位置与 momo 画面交替叠放。
     * viewId 为经 momo/platform_views 通道 create 过的视图;未知 id 在合成时被忽略。
     * 视图之后添加的内容画在视图之上。
     */
    addPlatformView(viewId: number, x: number, y: number, width: number, height: number): this;
    /** 未 pop 的层自动闭合;之后 builder 清空可复用。 */
    build(): Scene;
  }

  // ---------------------------------------------------------------------------
  // 输入(M2)。

  /**
   * 指针包: 每个事件占 POINTER_DATA_STRIDE(= 17)个 Float64, 字段顺序:
   *   0 timeStampUs  1 kind(0 mouse, 1 trackpad, 2 touch, 3 stylus)
   *   2 phase(0 add, 1 down, 2 move, 3 up, 4 remove(离开), 5 hover, 6 scroll, 7 gesture, 8 cancel(按下序列被取消))
   *   3 device  4 physicalX  5 physicalY(物理像素)  6 buttons(位图: 1 主键, 2 次键, 4 中键)
   *   7 scrollDeltaX  8 scrollDeltaY(物理像素)  9 momentum(1 = 惯性滚动阶段)
   *   10 screenX  11 screenY(屏幕位置, 逻辑点, 主屏左上原点;与宿主 momo/window 报告的窗口 frame 同一坐标系)
   *   12 sequenceId(宿主的按下序列 id, 0 = 不在序列内;momo/window startDrag / capturePointer 回送校验)
   *   13 signalPhase(scroll / gesture 事件的触控板手势阶段: 0 none(无阶段, 如普通滚轮) 1 start 2 update 3 end)
   *   14 gestureKind(gesture 事件: 1 magnify 2 rotate 3 smartMagnify 4 swipe 5 quickLook 6 pressure
   *      7 tabletPoint 8 begin 9 end)  15 gestureValue(magnify: 缩放增量;rotate: 角度;pressure / tabletPoint:
   *      压力 0..1)  16 gestureStage(pressure: Force Touch 阶段 0..2)。swipe 的方向在 7/8(scrollDelta)。
   * data 是复用的缓冲区, 仅在回调期间有效(需要保留时自行拷贝)。
   * (无运行期枚举导出: 框架层自行定义同值常量。)
   */
  export const POINTER_DATA_STRIDE: number;

  /** 修饰键位图: 1 Shift, 2 Control, 4 Alt/Option, 8 Meta/Command, 16 CapsLock。 */
  export interface KeyData {
    type: "down" | "up" | "repeat";
    timeStampUs: number;
    /** USB HID usage(page << 16 | usage), 如 KeyA = 0x00070004。 */
    physicalKey: number;
    /** 逻辑键: 可打印字符为其 Unicode 码点(小写);特殊键为 0x1_0000_0000 + HID usage。 */
    logicalKey: number;
    /** 该按键产生的字符(UTF-16), 无则为空串。 */
    character: string;
    /** KeyModifier 位图。 */
    modifiers: number;
  }

  export type SystemCursor =
    "basic" | "text" | "click" | "precise" | "resizeLeftRight" | "resizeUpDown" | "forbidden" | "none";

  /**
   * 一个视图(宿主的一个原生窗口)。多视图: 一个 JS 运行时驱动多个视图, 共享 JS 状态与帧时钟;每个视图有自己的
   * 尺寸、输入、焦点与画面。只有引擎职责(画面、尺寸、输入): 窗口的外观与行为(标题、光标、位置、标题栏、
   * 创建与关闭、指针序列的原生接收窗口…)属于宿主, 经宿主注册的 momo/window 通道操作。
   */
  export interface Viewport {
    readonly id: number;
    readonly devicePixelRatio: number;
    readonly physicalSize: { readonly width: number; readonly height: number };
    readonly focused: boolean;
    /** 窗口是否可见(隐藏或最小化时为 false: 引擎不为它出帧, 提交的画面被丢弃;重新可见时引擎补一帧)。 */
    readonly visible: boolean;
    onMetricsChanged: (() => void) | null;
    /** count 个指针事件, 见 POINTER_DATA_STRIDE。坐标为本视图的物理像素。 */
    onPointerPacket: ((data: Float64Array, count: number) => void) | null;
    /** 返回 true 表示已消费。 */
    onKeyData: ((data: KeyData) => boolean | void) | null;
    onFocusChanged: ((focused: boolean) => void) | null;
    onVisibilityChanged: ((visible: boolean) => void) | null;
    /** 提交本视图本帧的画面(只能在 onDrawFrame 内调用;每视图每帧一次)。 */
    render(scene: Scene): void;
  }

  /** 全局的帧调度与帧回调(对标 Flutter PlatformDispatcher;不是视图, 视图都在 viewports 里)。 */
  export interface PlatformDispatcher {
    /** frameTimeMs: 本帧 vsync 时间(毫秒, 单调时钟)。 */
    onBeginFrame: ((frameTimeMs: number) => void) | null;
    /** 在此对各视图调用 render(scene) 提交本帧;帧外调用 render 会被丢弃。 */
    onDrawFrame: (() => void) | null;
    /** viewports 增减(宿主打开或关闭了窗口)。 */
    onViewportsChanged: (() => void) | null;
    scheduleFrame(): void;
  }

  export const platformDispatcher: PlatformDispatcher;
  /**
   * 当前全部视图, 键为视图 id。视图 0 是宿主启动时创建的窗口(隐式视图: bundle 执行前即存在, runApp 渲染到
   * 这里);它与其他视图一样可以被关闭, 关闭后不在这里。
   */
  export const viewports: ReadonlyMap<number, Viewport>;
}

// 消息通道(docs/04 §5): 异步、字节流、双向。平台服务与扩展共用。
// 内置通道(载荷编码见 framework services/standard_codec.ts 与 engine/runtime/standard_codec.h):
// 引擎注册的: momo/platform_views、momo/extensions;宿主(embedder)注册的: momo/app、momo/window、
// momo/clipboard、momo/url、momo/textinput(应用、窗口与平台服务不经引擎, 见 embedder/common/*_channel.h)。
//   momo/app         双向       应用生命周期与退出(框架 services/app.ts 的 app;协议见 app_channel.h)
//   momo/window      双向       窗口(框架 services/widget.ts 的 Widget;协议见 window_channel.h)
//   momo/clipboard   JS→native  方法调用: getText() → string, setText(text)
//   momo/url         JS→native  方法调用: open(url)
//   momo/textinput   双向       JS→native: show(), hide(), setCaretRect([x,y,w,h]), setEditingState({...})
//                               native→JS: commit({text, replaceStart?, replaceEnd?}),
//                                          composing({text, cursor, replaceStart?, replaceEnd?}),
//                                          composingEnd(), command({command})
//                               replaceStart/End(UTF-16): 输入法显式指定的替换区间(重音字符/重新转换),
//                               缺省时替换 composing 区间, 否则替换选区。
//   momo/extensions  native→JS  扩展加载错误上报(string)
//   momo/platform_views  JS→native  create {id, viewType, params?: Uint8Array} / dispose {id} / focus {id}
//                        native→JS  focused {id}(用户点击等使平台视图获得键盘焦点)
// 方法调用编码(StandardMethodCodec, 与 Flutter 一致): 请求 = method(string 值)后紧接 args 值;
//   成功回复 = 字节 0 后接 result;错误回复 = 字节 1 后接 code、message、details(可省略)。
declare module "momo:services" {
  export type MessageHandler =
    (data: ArrayBuffer | null) => ArrayBuffer | null | void | Promise<ArrayBuffer | null | void>;
  /** 发送到原生侧, 回复为 handler 的返回数据;无 handler 时回复 null。 */
  export function send(channel: string, data: ArrayBuffer | null): Promise<ArrayBuffer | null>;
  /** 设置/清除 JS 侧 handler(原生发往该通道的消息由它处理)。 */
  export function setHandler(channel: string, handler: MessageHandler | null): void;
  /** 开发模式(--dev)。 */
  export const isDev: boolean;
  /** 当前平台: "macos" | "windows" | "linux" | "headless"。 */
  export const platform: string;
}
