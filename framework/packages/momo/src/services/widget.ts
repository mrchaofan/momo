// 原生窗口(对标 Chromium views::Widget / Electron BrowserWindow;协议见 embedder/common/window_channel.h 的
// momo/window)。每个打开着的窗口一个 Widget 对象: 方法操作窗口, 事件报告窗口的变化。窗口的外观与行为属于宿主,
// 不经引擎——引擎只认识它的视图(widget.viewport: 画面、尺寸、输入)。内容由 RootView 渲染进窗口。
//
//   const win = await Widget.create({ title: "Editor", width: 800, height: 600, show: false });
//   win.once("ready-to-show", () => win.show());
//   win.on("close", (e) => { if (dirty) e.preventDefault(); });
//
// 对象的生命周期: 窗口打开期间同一个 id 总是同一个 Widget;窗口关闭('closed')后对象作废(isDestroyed),
// 方法变为空操作。JS 丢掉引用不会关闭窗口。
// 状态缓存: 屏幕框(bounds)、窗口控制按钮占用区域(controls)、状态(state)与能力(capabilities)首次访问时向宿主
// 查询(getState), 之后由宿主推送更新。宿主没有注册 momo/window(headless 测试运行器)时方法是空操作。
import { viewports, type Viewport } from "momo:ui";
import { reportError } from "../foundation/assert";
import { EventEmitter, HostEvent } from "../foundation/event_emitter";
import { MissingPluginException, PlatformException, type MethodCall } from "./message_codecs";
import { MethodChannel } from "./platform_channel";

/** 屏幕上的矩形(逻辑点, 主屏左上原点, y 向下)。 */
export interface WidgetBounds {
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
}

/** 窗口状态。visible: 已显示(hide 后为 false;最小化的窗口仍为 true)。 */
export interface WidgetState {
  readonly visible: boolean;
  readonly minimized: boolean;
  readonly maximized: boolean;
  readonly fullscreen: boolean;
}

/** 宿主窗口的可选能力(不支持的能力对应的方法是空操作)。 */
export type WidgetCapability =
  | "alwaysOnTop" | "position" | "resizeOptions" | "hiddenTitleBar" | "windowDrag" | "moveLoop"
  | "pointerCapture" | "platformViews" | "nonActivating" | "visibility" | "minMax";

/** 打开窗口的参数(逻辑点)。 */
export interface WidgetOptions {
  title?: string;
  width: number;
  height: number;
  minWidth?: number;
  minHeight?: number;
  /** 内容区左上角的屏幕位置(x 与 y 同时给出时生效;缺省由宿主居中)。 */
  x?: number;
  y?: number;
  /**
   * 辅助窗口: 可接收键盘, 但不成为应用的主窗口, 聚焦它也不激活应用(macOS: NSPanel 非激活面板;
   * 不支持的平台按普通窗口处理)。
   */
  nonActivating?: boolean;
  /** 用户能否拖动边框缩放窗口。缺省 true。 */
  resizable?: boolean;
  /** 创建后立即显示。缺省 true。false: 第一帧画好后触发 'ready-to-show', 应用再 show()(避免先看到空白窗口)。 */
  show?: boolean;
}

/**
 * 窗口内侧的系统缩放边框(逻辑点;数字为四边相同, 未给出的边保持不变)。在这里光标显示为缩放形状、按下交给
 * 系统缩放, 优先于应用内容;0 = 该边没有内侧边框(系统在窗口外侧的缩放区仍可用)。宿主默认 5。
 */
export type WidgetResizeBorder = number | { left?: number; top?: number; right?: number; bottom?: number };

/** 标题栏外观。 */
export interface WidgetTitleBarOptions {
  /** 隐藏系统标题栏: 内容铺满窗口, 系统窗口控制按钮浮在内容之上(占用区域见 controls)。 */
  hidden: boolean;
  /** 应用自绘标题栏的高度(逻辑点): 宿主让系统控制按钮在这个高度内垂直居中。缺省系统位置。 */
  height?: number;
}

export type WidgetEvents = {
  /** 用户点了关闭按钮(或 close()、应用退出)。preventDefault() 取消关闭。 */
  "close": [event: HostEvent];
  /** 窗口已关闭: 对象作废, 应用应把它从状态里移除。 */
  "closed": [];
  "focus": [];
  "blur": [];
  "show": [];
  "hide": [];
  "minimize": [];
  "maximize": [];
  /** 从最小化或最大化恢复。 */
  "restore": [];
  "state-changed": [state: WidgetState];
  /** 内容区在屏幕上的位置变化。 */
  "move": [bounds: WidgetBounds];
  /** 内容区尺寸变化。 */
  "resize": [bounds: WidgetBounds];
  /** 系统窗口控制按钮占用的区域变化(标题栏未隐藏时为 null)。 */
  "controls-changed": [controls: WidgetBounds | null];
  /** 第一帧已画好(以 show: false 创建的窗口在此 show())。 */
  "ready-to-show": [];
};

const kDefaultState: WidgetState = { visible: true, minimized: false, maximized: false, fullscreen: false };

function toRect(v: unknown): WidgetBounds | null {
  if (v === null || v === undefined || typeof (v as ArrayLike<number>).length !== "number") return null;
  const a = v as ArrayLike<number>;
  if (a.length < 4) return null;
  return { x: Number(a[0]), y: Number(a[1]), width: Number(a[2]), height: Number(a[3]) };
}

function toState(v: unknown): WidgetState | null {
  if (v === null || typeof v !== "object") return null;
  const r = v as Record<string, unknown>;
  return { visible: r["visible"] === true, minimized: r["minimized"] === true, maximized: r["maximized"] === true,
           fullscreen: r["fullscreen"] === true };
}

/** 宿主不支持(通道未注册, 或该能力不可用)、或窗口已关闭: 调用方可忽略。 */
function isUnsupported(e: unknown): boolean {
  return e instanceof MissingPluginException || (e instanceof PlatformException && (e.code === "unimplemented" || e.code === "notFound"));
}

const channel = new MethodChannel("momo/window");

/** 调用宿主方法;宿主不支持时返回 null(不报错), 其他错误报告后返回 null。 */
async function invoke<R>(method: string, args: Record<string, unknown>): Promise<R | null> {
  try {
    return await channel.invokeMethod<R>(method, args);
  } catch (e) {
    if (!isUnsupported(e)) reportError(e, `calling momo/window ${method}`);
    return null;
  }
}

/** 一个原生窗口。 */
export class Widget extends EventEmitter<WidgetEvents> {
  static readonly #widgets = new Map<number, Widget>();
  static #initialized = false;

  /** 打开窗口。视图在返回之前已出现在 momo:ui 的 viewports 里。宿主不支持多窗口时 reject。 */
  static async create(options: WidgetOptions): Promise<Widget> {
    Widget.#ensureInitialized();
    const id = await channel.invokeMethod<number>("create", { ...options });
    if (typeof id !== "number") throw new Error("momo/window create: the host returned no view id");
    const widget = Widget.fromId(id);
    if (widget === null) throw new Error("momo/window create: the window was closed before it opened");
    return widget;
  }
  /** 视图 id 对应的窗口(含宿主启动时创建的视图 0);窗口不存在时为 null。 */
  static fromId(id: number): Widget | null {
    Widget.#ensureInitialized();
    let widget = Widget.#widgets.get(id);
    if (widget === undefined) {
      if (!viewports.has(id)) return null;
      widget = new Widget(id);
      Widget.#widgets.set(id, widget);
    }
    return widget;
  }
  /** 全部打开着的窗口(按 id 升序)。 */
  static getAll(): Widget[] {
    return [...viewports.keys()].sort((a, b) => a - b).map((id) => Widget.fromId(id))
      .filter((w): w is Widget => w !== null);
  }
  /** 获得系统焦点的窗口。 */
  static getFocused(): Widget | null {
    for (const [id, viewport] of viewports) {
      if (viewport.focused) return Widget.fromId(id);
    }
    return null;
  }

  /** 测试用: 作废全部对象(不关闭窗口)。 */
  static debugReset(): void {
    for (const w of Widget.#widgets.values()) w.removeAllListeners();
    Widget.#widgets.clear();
  }

  static #ensureInitialized(): void {
    if (Widget.#initialized) return;
    Widget.#initialized = true;
    channel.setMethodCallHandler((call) => Widget.#dispatch(call));
  }

  static async #dispatch(call: MethodCall): Promise<unknown> {
    const args = (call.args ?? {}) as Record<string, unknown>;
    const id = typeof args["view"] === "number" ? args["view"] : -1;
    // closeRequested 对宿主创建、JS 尚未取过对象的窗口也要有回答: 按需建对象(无监听器即允许)。
    const widget = call.method === "closed" ? Widget.#widgets.get(id) : Widget.fromId(id);
    if (widget === undefined || widget === null) return call.method === "closeRequested" ? true : null;
    return widget.#handle(call.method, args);
  }

  readonly id: number;
  #destroyed = false;
  #bounds: WidgetBounds | null = null;
  #controls: WidgetBounds | null = null;
  #state: WidgetState = kDefaultState;
  #capabilities: ReadonlySet<WidgetCapability> | null = null;
  #loading: Promise<void> | null = null;
  /** 宿主推送的更新计数: 查询发出之后到达的推送比查询的回复新, 回复里的同名字段不再覆盖它。 */
  #version = 0;
  #queried = false;

  private constructor(id: number) {
    super();
    this.id = id;
  }

  // ---- 状态 ----

  /** 窗口的视图(引擎侧: 画面、尺寸、输入);窗口关闭后为 null。 */
  get viewport(): Viewport | null {
    return this.#destroyed ? null : viewports.get(this.id) ?? null;
  }
  isDestroyed(): boolean {
    return this.#destroyed;
  }
  /** 内容区在屏幕上的框;宿主不支持或尚未取得时为 null。首次访问触发查询。 */
  get bounds(): WidgetBounds | null {
    this.#load();
    return this.#bounds;
  }
  /** 系统窗口控制按钮在内容区里占用的区域(逻辑点, 内容区左上原点);标题栏未隐藏时为 null。 */
  get controls(): WidgetBounds | null {
    this.#load();
    return this.#controls;
  }
  get state(): WidgetState {
    this.#load();
    return this.#state;
  }
  get isVisible(): boolean {
    return this.state.visible;
  }
  get isMinimized(): boolean {
    return this.state.minimized;
  }
  get isMaximized(): boolean {
    return this.state.maximized;
  }
  get isFocused(): boolean {
    return this.viewport?.focused ?? false;
  }
  /** 可选能力;尚未取得时为 null。 */
  get capabilities(): ReadonlySet<WidgetCapability> | null {
    this.#load();
    return this.#capabilities;
  }
  /** 向宿主重新查询状态(bounds / controls / state / capabilities)。 */
  refresh(): Promise<void> {
    this.#queried = true;
    const version = this.#version;
    this.#loading ??= invoke<Record<string, unknown>>("getState", { view: this.id }).then((r) => {
      this.#loading = null;
      if (r === null || this.#destroyed) return;
      if (this.#version === version) {
        this.#bounds = toRect(r["frame"]);
        this.#controls = toRect(r["controls"]);
        this.#state = toState(r["state"]) ?? this.#state;
      }
      const caps = Array.isArray(r["capabilities"]) ? (r["capabilities"] as WidgetCapability[]) : [];
      this.#capabilities = new Set(caps);
      this.emit("state-changed", this.#state);
    });
    return this.#loading;
  }

  // ---- 方法(JS → 宿主;窗口已关闭或宿主不支持时为空操作) ----

  /** 关闭窗口: 先触发 'close'(可取消), 与用户点关闭按钮相同。 */
  close(): void {
    this.#call("requestClose");
  }
  /** 立即关闭, 不触发 'close'。 */
  destroy(): void {
    this.#call("close");
  }
  show(): void {
    this.#call("show");
  }
  hide(): void {
    this.#call("hide");
  }
  /** 把窗口带到最前并获得焦点(最小化时先还原)。 */
  focus(): void {
    this.#call("activate");
  }
  minimize(): void {
    this.#call("minimize");
  }
  maximize(): void {
    this.#call("maximize");
  }
  /** 从最小化或最大化恢复。 */
  restore(): void {
    this.#call("restore");
  }
  setTitle(title: string): void {
    this.#call("setTitle", { title });
  }
  setMinimumSize(width: number, height: number): void {
    this.#call("setMinSize", { width, height });
  }
  setCursor(cursor: string): void {
    this.#call("setCursor", { cursor });
  }
  setAlwaysOnTop(on: boolean): void {
    this.#call("setAlwaysOnTop", { on });
  }
  /** 移动窗口, 使内容区左上角位于屏幕位置 (x, y)。 */
  setPosition(x: number, y: number): void {
    this.#call("setPosition", { x, y });
  }
  /** 用户能否拖动边框缩放窗口。 */
  setResizable(resizable: boolean): void {
    this.#call("setResizable", { resizable });
  }
  /** 窗口内侧的系统缩放边框(与 setResizable 互不覆盖)。 */
  setResizeBorder(border: WidgetResizeBorder): void {
    const edges = typeof border === "number" ? { left: border, top: border, right: border, bottom: border } : border;
    this.#call("setResizeBorder", { ...edges });
  }
  setTitleBar(options: WidgetTitleBarOptions): void {
    this.#call("setTitleBar", { hidden: options.hidden, height: options.height ?? 0 });
  }
  /**
   * 以本窗口里进行中的按下序列开始系统的窗口拖动(含贴边、跨屏等系统行为;双击按系统设置缩放/最小化)。
   * 哪里可以拖动由应用决定: 通常在按下时调用, sequenceId 取自那次按下(PointerDownEvent.sequenceId)。
   * 序列已结束(用户已松开)时宿主什么也不做;拖动结束后序列随之结束。
   */
  startDrag(sequenceId: number): void {
    this.#call("startDrag", { sequenceId });
  }
  /**
   * 移动循环: 直到按键松开(或 endMoveLoop), 宿主在每个鼠标事件上同步移动窗口, 使内容区左上角位于
   * 指针屏幕位置 − (grabX, grabY)。窗口跟随指针由宿主完成, 不经 ui 线程往返。
   */
  startMoveLoop(grabX: number, grabY: number): void {
    this.#call("startMoveLoop", { x: grabX, y: grabY });
  }
  endMoveLoop(): void {
    this.#call("endMoveLoop");
  }
  /**
   * 进行中的按下序列 sequenceId 改由本窗口接收(宿主的原生捕获)。框架里用 PointerCapture.moveToView, 它同时
   * 保证逻辑接收方不变;单独调用只改原生事件的来源窗口。序列已结束或不匹配时宿主什么也不做。
   */
  capturePointer(sequenceId: number): void {
    this.#call("capturePointer", { sequenceId });
  }

  // ---- 内部 ----

  #call(method: string, args: Record<string, unknown> = {}): void {
    if (this.#destroyed) return;
    void invoke(method, { view: this.id, ...args });
  }

  #load(): void {
    if (!this.#queried && !this.#destroyed) void this.refresh();
  }

  async #handle(method: string, args: Record<string, unknown>): Promise<unknown> {
    switch (method) {
      case "frameChanged": {
        const old = this.#bounds;
        const bounds = toRect(args["frame"]);
        this.#bounds = bounds;
        this.#version++;
        if (bounds !== null) {
          if (old === null || old.x !== bounds.x || old.y !== bounds.y) this.emit("move", bounds);
          if (old === null || old.width !== bounds.width || old.height !== bounds.height) this.emit("resize", bounds);
        }
        return null;
      }
      case "controlsChanged":
        this.#controls = toRect(args["controls"]);
        this.#version++;
        this.emit("controls-changed", this.#controls);
        return null;
      case "stateChanged": {
        const next = toState(args["state"]);
        if (next === null) return null;
        const old = this.#state;
        this.#state = next;
        this.#version++;
        if (next.visible !== old.visible) this.emit(next.visible ? "show" : "hide");
        if (next.minimized && !old.minimized) this.emit("minimize");
        if (next.maximized && !old.maximized) this.emit("maximize");
        if ((old.minimized && !next.minimized) || (old.maximized && !next.maximized && !next.minimized)) {
          this.emit("restore");
        }
        this.emit("state-changed", next);
        return null;
      }
      case "focusChanged":
        this.emit(args["focused"] === true ? "focus" : "blur");
        return null;
      case "readyToShow":
        this.emit("ready-to-show");
        return null;
      case "closeRequested": {
        const e = new HostEvent();
        this.emit("close", e);
        return e.settle();
      }
      case "closed":
        this.#destroyed = true;
        Widget.#widgets.delete(this.id);
        this.emit("closed");
        this.removeAllListeners();
        return null;
      default:
        return null;
    }
  }
}
