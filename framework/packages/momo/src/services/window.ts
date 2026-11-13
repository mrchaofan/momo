// 窗口管理服务(对标 Flutter 的 SystemChannels + 桌面 windowing 通道): 封装宿主注册的 momo/window 与
// momo/app 通道。窗口的外观与行为属于宿主(embedder), 不经过引擎: 引擎的 momo:ui 只认识视图的画面、
// 尺寸与输入, 这里负责标题、位置、标题栏、拖动、关闭等。
//
// 状态缓存: 每个视图的屏幕框(frame)、系统窗口控制按钮占用的区域(controls)与平台能力(capabilities)。
// 首次访问一个视图时向宿主查询(getState), 之后由宿主的 frameChanged / controlsChanged 推送更新。
// 宿主没有注册该通道(headless 测试运行器)时所有调用都是空操作, 状态保持为空。
import { reportError } from "../foundation/assert";
import { MissingPluginException, PlatformException, type MethodCall } from "./message_codecs";
import { MethodChannel } from "./platform_channel";

/** 屏幕上的矩形(逻辑点, 主屏左上原点, y 向下)。 */
export interface WindowFrame {
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly height: number;
}

/** 宿主窗口的可选能力(不支持的能力对应的调用是空操作)。 */
export type WindowCapability =
  | "alwaysOnTop" | "position" | "resizeOptions" | "hiddenTitleBar" | "windowDrag" | "moveLoop"
  | "pointerCapture" | "platformViews" | "nonActivating";

/** 打开窗口的参数(逻辑点)。 */
export interface WindowCreateOptions {
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
}

/**
 * 窗口内侧的系统缩放边框(逻辑点;数字为四边相同, 未给出的边保持不变)。在这里光标显示为缩放形状、按下交给
 * 系统缩放, 优先于应用内容;0 = 该边没有内侧边框(系统在窗口外侧的缩放区仍可用)。宿主默认 5。
 */
export type WindowResizeBorder = number | { left?: number; top?: number; right?: number; bottom?: number };

/** 标题栏外观。 */
export interface WindowTitleBarOptions {
  /** 隐藏系统标题栏: 内容铺满窗口, 系统窗口控制按钮浮在内容之上(占用区域见 controlsOf)。 */
  hidden: boolean;
  /** 应用自绘标题栏的高度(逻辑点): 宿主让系统控制按钮在这个高度内垂直居中。缺省系统位置。 */
  height?: number;
}

/** 窗口状态变化的种类。 */
export type WindowChange = "frame" | "controls" | "state";

interface ViewState {
  frame: WindowFrame | null;
  controls: WindowFrame | null;
  capabilities: ReadonlySet<WindowCapability> | null;
  /** getState 进行中(去重)。 */
  loading: Promise<void> | null;
  /** 宿主推送的更新计数: 查询发出之后到达的推送比查询的回复新, 回复里的同名字段不再覆盖它。 */
  version: number;
  /** 已向宿主查询过(不论成功与否;宿主不支持时不反复查询)。 */
  queried: boolean;
}

function toRect(v: unknown): WindowFrame | null {
  if (v === null || v === undefined || typeof (v as ArrayLike<number>).length !== "number") return null;
  const a = v as ArrayLike<number>;
  if (a.length < 4) return null;
  return { x: Number(a[0]), y: Number(a[1]), width: Number(a[2]), height: Number(a[3]) };
}

/** 宿主不支持(通道未注册, 或该能力不可用): 调用方可忽略。 */
function isUnsupported(e: unknown): boolean {
  return e instanceof MissingPluginException || (e instanceof PlatformException && (e.code === "unimplemented" || e.code === "notFound"));
}

/** 宿主的窗口管理服务(单例)。 */
export class WindowService {
  static #instance: WindowService | null = null;
  static get instance(): WindowService {
    if (WindowService.#instance === null) WindowService.#instance = new WindowService();
    return WindowService.#instance;
  }

  readonly #channel = new MethodChannel("momo/window");
  readonly #appChannel = new MethodChannel("momo/app");
  readonly #states = new Map<number, ViewState>();
  readonly #listeners = new Set<(view: number, change: WindowChange) => void>();
  readonly #closeHandlers = new Map<number, () => boolean | void | Promise<boolean | void>>();
  #exitHandler: (() => boolean | void | Promise<boolean | void>) | null = null;
  #initialized = false;

  private constructor() {}

  /** 注册宿主 → JS 的处理器(frameChanged / controlsChanged / closeRequested、momo/app requestExit)。 */
  ensureInitialized(): void {
    if (this.#initialized) return;
    this.#initialized = true;
    this.#channel.setMethodCallHandler((call) => this.#handle(call));
    this.#appChannel.setMethodCallHandler(async (call) => {
      if (call.method !== "requestExit") return null;
      const handler = this.#exitHandler;
      return handler === null ? true : (await handler()) !== false;
    });
  }

  // ---- 状态 ----

  /** 视图(窗口)内容区在屏幕上的框;宿主不支持或尚未取得时为 null。首次访问触发查询。 */
  frameOf(view: number): WindowFrame | null {
    return this.#state(view).frame;
  }
  /** 系统窗口控制按钮在内容区里占用的区域(逻辑点, 内容区左上原点);标题栏未隐藏时为 null。 */
  controlsOf(view: number): WindowFrame | null {
    return this.#state(view).controls;
  }
  /** 视图的可选能力;尚未取得时为 null。 */
  capabilitiesOf(view: number): ReadonlySet<WindowCapability> | null {
    return this.#state(view).capabilities;
  }
  /** 向宿主重新查询视图的状态(frame / controls / capabilities)。 */
  refresh(view: number): Promise<void> {
    const state = this.#state(view, false);
    state.queried = true;
    const version = state.version;
    state.loading ??= this.#invoke<Record<string, unknown>>("getState", { view }).then((r) => {
      state.loading = null;
      if (r === null || this.#states.get(view) !== state) return;
      if (state.version === version) {
        state.frame = toRect(r["frame"]);
        state.controls = toRect(r["controls"]);
      }
      const caps = Array.isArray(r["capabilities"]) ? (r["capabilities"] as WindowCapability[]) : [];
      state.capabilities = new Set(caps);
      this.#notify(view, "state");
    });
    return state.loading;
  }
  /** 监听窗口状态变化(frame / controls / state = 首次查询完成)。 */
  addListener(listener: (view: number, change: WindowChange) => void): void {
    this.#listeners.add(listener);
  }
  removeListener(listener: (view: number, change: WindowChange) => void): void {
    this.#listeners.delete(listener);
  }
  /** 视图已关闭: 丢弃它的状态与处理器(由 WidgetsBinding 在视图集合变化时调用)。 */
  forget(view: number): void {
    this.#states.delete(view);
    this.#closeHandlers.delete(view);
  }

  // ---- 关闭与退出 ----

  /** 用户点了视图 view 的关闭按钮时询问 handler(返回 false 取消)。null 移除(= 允许关闭)。 */
  setCloseRequestHandler(view: number, handler: (() => boolean | void | Promise<boolean | void>) | null): void {
    if (handler === null) this.#closeHandlers.delete(view);
    else this.#closeHandlers.set(view, handler);
  }
  /** 用户/系统请求退出应用(Cmd+Q 等)时询问 handler(返回 false 取消)。 */
  setAppExitHandler(handler: (() => boolean | void | Promise<boolean | void>) | null): void {
    this.#exitHandler = handler;
  }

  // ---- JS → 宿主(异步;宿主不支持时为空操作) ----

  /** 打开窗口, 返回新视图 id(视图在此之前已出现在 momo:ui 的 views 里)。宿主不支持时 reject。 */
  async create(options: WindowCreateOptions): Promise<number> {
    const id = await this.#channel.invokeMethod<number>("create", { ...options });
    if (typeof id !== "number") throw new Error("momo/window create: the host returned no view id");
    return id;
  }
  /** 关闭窗口(不询问 closeRequested)。 */
  close(view: number): void {
    void this.#invoke("close", { view });
  }
  setTitle(view: number, title: string): void {
    void this.#invoke("setTitle", { view, title });
  }
  setMinSize(view: number, width: number, height: number): void {
    void this.#invoke("setMinSize", { view, width, height });
  }
  setCursor(view: number, cursor: string): void {
    void this.#invoke("setCursor", { view, cursor });
  }
  setAlwaysOnTop(view: number, on: boolean): void {
    void this.#invoke("setAlwaysOnTop", { view, on });
  }
  /** 把窗口带到最前并获得焦点(最小化时先还原)。 */
  activate(view: number): void {
    void this.#invoke("activate", { view });
  }
  /** 移动窗口, 使内容区左上角位于屏幕位置 (x, y)。 */
  setPosition(view: number, x: number, y: number): void {
    void this.#invoke("setPosition", { view, x, y });
  }
  /** 用户能否拖动边框缩放窗口。 */
  setResizable(view: number, resizable: boolean): void {
    void this.#invoke("setResizable", { view, resizable });
  }
  /** 窗口内侧的系统缩放边框(与 setResizable 互不覆盖)。 */
  setResizeBorder(view: number, border: WindowResizeBorder): void {
    const edges = typeof border === "number" ? { left: border, top: border, right: border, bottom: border } : border;
    void this.#invoke("setResizeBorder", { view, ...edges });
  }
  setTitleBar(view: number, options: WindowTitleBarOptions): void {
    void this.#invoke("setTitleBar", { view, hidden: options.hidden, height: options.height ?? 0 });
  }
  /**
   * 以窗口 view 里进行中的按下序列开始系统的窗口拖动(含贴边、跨屏等系统行为;双击按系统设置缩放/最小化)。
   * 哪里可以拖动由应用决定(框架不提供拖动区域): 通常在按下时调用, sequenceId 取自那次按下
   * (PointerDownEvent.sequenceId)。序列已结束(用户已松开)时宿主什么也不做;拖动结束后序列随之结束。
   */
  startDrag(view: number, sequenceId: number): void {
    void this.#invoke("startDrag", { view, sequenceId });
  }
  /**
   * 移动循环: 直到按键松开(或 endMoveLoop), 宿主在每个鼠标事件上同步移动窗口, 使内容区左上角位于
   * 指针屏幕位置 − (grabX, grabY)。窗口跟随指针由宿主完成, 不经 ui 线程往返。
   */
  startMoveLoop(view: number, grabX: number, grabY: number): void {
    void this.#invoke("startMoveLoop", { view, x: grabX, y: grabY });
  }
  endMoveLoop(view: number): void {
    void this.#invoke("endMoveLoop", { view });
  }
  /**
   * 进行中的按下序列 sequenceId 改由窗口 view 接收(宿主的原生捕获)。框架里用 PointerCapture.moveToView, 它同时
   * 保证逻辑接收方不变;单独调用只改原生事件的来源窗口。序列已结束或不匹配时宿主什么也不做。
   */
  capturePointer(view: number, sequenceId: number): void {
    void this.#invoke("capturePointer", { view, sequenceId });
  }

  /** 测试用: 清空缓存的状态与处理器。 */
  debugReset(): void {
    this.#states.clear();
    this.#closeHandlers.clear();
    this.#exitHandler = null;
  }

  // ---- 内部 ----

  #state(view: number, load = true): ViewState {
    let state = this.#states.get(view);
    if (state === undefined) {
      state = { frame: null, controls: null, capabilities: null, loading: null, version: 0, queried: false };
      this.#states.set(view, state);
    }
    if (load && !state.queried) void this.refresh(view);
    return state;
  }

  #notify(view: number, change: WindowChange): void {
    for (const l of [...this.#listeners]) {
      try {
        l(view, change);
      } catch (e) {
        reportError(e, "in a window state listener");
      }
    }
  }

  async #handle(call: MethodCall): Promise<unknown> {
    const args = (call.args ?? {}) as Record<string, unknown>;
    const view = typeof args["view"] === "number" ? args["view"] : -1;
    switch (call.method) {
      case "frameChanged": {
        const state = this.#state(view, false);
        state.frame = toRect(args["frame"]);
        state.version++;
        this.#notify(view, "frame");
        return null;
      }
      case "controlsChanged": {
        const state = this.#state(view, false);
        state.controls = toRect(args["controls"]);
        state.version++;
        this.#notify(view, "controls");
        return null;
      }
      case "closeRequested": {
        const handler = this.#closeHandlers.get(view);
        if (handler === undefined) return true;
        try {
          return (await handler()) !== false;
        } catch (e) {
          reportError(e, "in a window close request handler");
          return true;  // 处理器出错不阻止关闭
        }
      }
      default:
        return null;
    }
  }

  /** 调用宿主方法;宿主不支持时返回 null(不报错), 其他错误报告后返回 null。 */
  async #invoke<R>(method: string, args: Record<string, unknown>): Promise<R | null> {
    try {
      return await this.#channel.invokeMethod<R>(method, args);
    } catch (e) {
      if (!isUnsupported(e)) reportError(e, `calling momo/window ${method}`);
      return null;
    }
  }
}
