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
   *   2 phase(0 add, 1 down, 2 move, 3 up, 4 remove, 5 hover, 6 scroll, 7 gesture)
   *   3 device  4 physicalX  5 physicalY(物理像素)  6 buttons(位图: 1 主键, 2 次键, 4 中键)
   *   7 scrollDeltaX  8 scrollDeltaY(物理像素)  9 momentum(1 = 惯性滚动阶段)
   *   10 screenX  11 screenY(屏幕位置, 逻辑点, 主屏左上原点;与 View.screenFrame 同一坐标系)
   *   12 nativeEventId(宿主为原生事件分配的 id, 0 = 合成事件;见 View.dispatchPlatformViewPointer)
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
   * 一个视图(原生窗口)。多视图: 一个 JS 运行时驱动多个窗口, 共享 JS 状态与帧时钟;每个窗口有自己的
   * 尺寸、输入、焦点与画面。视图 0 是主窗口(= window)。
   */
  export interface View {
    readonly id: number;
    readonly devicePixelRatio: number;
    readonly physicalSize: { readonly width: number; readonly height: number };
    readonly focused: boolean;
    /**
     * 内容区(本视图)在屏幕上的框(逻辑点, 主屏左上原点, y 向下);宿主未报告时全为 0。
     * 以内容区为准: PointerEvent.screenPosition − screenFrame 的原点 = 窗口内坐标, 与窗口样式无关。
     */
    readonly screenFrame: { readonly x: number; readonly y: number; readonly width: number; readonly height: number };
    /** 只写。 */
    title: string;
    onMetricsChanged: (() => void) | null;
    /** 窗口移动或改变尺寸(screenFrame 已更新)。 */
    onFrameChanged: (() => void) | null;
    /** 返回 false 取消关闭(如"未保存"提示);抛错视为放行。主窗口: 关闭 = 退出应用。 */
    onCloseRequested: (() => boolean | void) | null;
    /** count 个指针事件, 见 POINTER_DATA_STRIDE。坐标为本窗口的物理像素。 */
    onPointerPacket: ((data: Float64Array, count: number) => void) | null;
    /** 返回 true 表示已消费。 */
    onKeyData: ((data: KeyData) => boolean | void) | null;
    onFocusChanged: ((focused: boolean) => void) | null;
    /** 提交本视图本帧的画面(只能在 onDrawFrame 内调用;每视图每帧一次)。 */
    render(scene: Scene): void;
    setCursor(cursor: SystemCursor): void;
    /** 窗口最小尺寸(逻辑像素)。 */
    setMinSize(width: number, height: number): void;
    /** 关闭窗口(不询问 onCloseRequested)。主窗口不能关闭(抛错)。 */
    close(): void;
    /** 窗口浮在普通窗口之上(macOS: 浮动层级, 跟随桌面空间)。 */
    setAlwaysOnTop(on: boolean): void;
    /** 把窗口带到最前并获得焦点(最小化时先还原)。 */
    activate(): void;
    /**
     * 把进行中的鼠标按下序列转交给本窗口(类似 Win32 SetCapture / Chromium Widget::SetCapture)。
     * 原窗口此后不再收到这次按下的事件;本窗口在当前指针位置收到一次按下, 之后的拖动与抬起都投递给它
     * (即使按下所在的窗口随后关闭), 抬起后自动释放。返回是否有序列被转交(没有按住的按键时为 false)。
     * 框架层通常用 GestureBinding.transferPointerCapture, 它同时把被转交的序列交给调用方。
     */
    setCapture(): boolean;
    /**
     * 框架对一个原生指针事件的处置(docs/05 §9 "输入"), 宿主异步执行:
     *   action 0(event): 把 nativeEventId 对应的原生事件投递给平台视图 platformViewId(按下时在原生视图内
     *     捕获);platformViewId 为 0 表示该按下由 momo 内容消费(宿主把键盘焦点交还 momo)。
     *   action 1(enter)/ 2(exit): 指针归属移到 / 离开平台视图 platformViewId;nativeEventId 为触发它的原生
     *     事件(宿主取其位置与时间, 0 = 取当前指针位置)。与 action 0 按调用顺序执行: 原生视图看到的进入、
     *     事件、离开与框架处理它们的顺序一致。
     * 框架层通过 GestureBinding 使用, 一般不直接调用。
     */
    dispatchPlatformViewPointer(platformViewId: number, nativeEventId: number, action: number): void;
    /**
     * "hidden": 无边框外观——隐藏系统标题栏, 内容铺满整个窗口, 保留红绿灯(窗口控制按钮);
     * trafficLightsCenterY: 红绿灯中心距窗口顶部的逻辑像素(缺省系统位置)。"default": 恢复系统标题栏。
     * 隐藏标题栏后窗口靠 setDragRegions 声明的区域拖动。
     */
    setTitleBarStyle(style: "default" | "hidden", trafficLightsCenterY?: number): void;
    /** 可拖动窗口的区域: [x, y, width, height, ...](逻辑像素, 左上原点);在其中拖动移动窗口, 双击缩放。 */
    setDragRegions(rects: readonly number[]): void;
    /** 移动窗口, 使内容区左上角位于屏幕位置 (x, y)(逻辑点, 主屏左上原点)。 */
    setPosition(x: number, y: number): void;
  }

  /** 主窗口(视图 0), 同时承载全局帧回调。 */
  export interface Window extends View {
    /** frameTimeMs: 本帧 vsync 时间(毫秒, 单调时钟)。 */
    onBeginFrame: ((frameTimeMs: number) => void) | null;
    /** 在此对各视图调用 render(scene) 提交本帧;帧外调用 render 会被丢弃。 */
    onDrawFrame: (() => void) | null;
    /** views 增减(createView 完成、窗口被关闭)。 */
    onViewsChanged: (() => void) | null;
    scheduleFrame(): void;
  }

  export const window: Window;
  /** 当前全部视图(含视图 0), 键为视图 id。 */
  export const views: ReadonlyMap<number, View>;
  /** 打开一个新窗口(逻辑像素)。宿主不支持或创建失败时 reject。 */
  /**
   * x/y: 内容区左上角的屏幕位置(逻辑点, 主屏左上原点), 两者同时给出时生效, 缺省居中。
   * panel: 面板(同 Electron 的 type: 'panel';macOS 为 NSPanel + NonactivatingPanel)——可成为 key window
   * 接收键盘, 但不成为 main window, 聚焦它也不激活应用(应用在后台时不被带到前台, 其他窗口保持原状)。缺省 false。
   */
  export function createView(options: { title?: string; width: number; height: number; minWidth?: number; minHeight?: number; x?: number; y?: number; panel?: boolean }): Promise<View>;
}

// 消息通道(docs/04 §5): 异步、字节流、双向。平台服务与扩展共用。
// 内置通道(载荷编码见 framework services/standard_codec.ts 与 engine/runtime/standard_codec.h):
//   momo/lifecycle   native→JS  string: "resumed" | "suspended" | "terminating"
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
