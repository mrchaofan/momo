// 应用(对标 Electron app;协议见 embedder/common/app_channel.h 的 momo/app)。应用生命周期属于宿主, 与窗口无关:
// 所有窗口关闭后应用照常运行(只发 'window-all-closed'), 是否退出由应用决定。
//
//   app.quit()       请求退出: 'before-quit' → 逐个询问窗口('close')→ 'will-quit' → 退出;任一步取消即中止
//   app.exit(code)   立即退出(不询问), 仍有序关闭;process.exit(code) 走同一路径
//   app.hide() / app.show() / app.focus({ steal })
//
// 宿主没有注册 momo/app(headless 测试运行器)时方法是空操作, 状态停在 "launching"。
import { reportError } from "../foundation/assert";
import { EventEmitter, HostEvent } from "../foundation/event_emitter";
import { MissingPluginException, PlatformException, type MethodCall } from "./message_codecs";
import { MethodChannel } from "./platform_channel";

/** 应用状态。suspended = 应用整体被隐藏(不出帧);窗口的最小化/隐藏不改变应用状态。 */
export type AppState = "launching" | "running" | "suspended" | "terminating";

/** app 的可选能力(不支持的方法是空操作)。 */
export type AppCapability = "hide" | "activate";

export type AppEvents = {
  /** 退出流程开始(Cmd+Q、菜单退出、app.quit())。preventDefault() 取消退出。 */
  "before-quit": [event: HostEvent];
  /** 所有窗口已关闭, 即将退出。preventDefault() 取消(应用无窗口继续运行)。 */
  "will-quit": [event: HostEvent];
  /** 应用正在退出(此后 JS 运行时随时销毁, 只适合同步收尾)。 */
  "quit": [];
  /** 最后一个窗口已关闭(退出流程中不触发)。不处理时应用继续运行。 */
  "window-all-closed": [];
  /** 用户重新激活应用(macOS 点击 Dock 图标)。通常在没有可见窗口时重新打开窗口。 */
  "activate": [info: { hasVisibleWindows: boolean }];
  "did-become-active": [];
  "did-resign-active": [];
  "state-changed": [state: AppState];
};

function isUnsupported(e: unknown): boolean {
  return e instanceof MissingPluginException || (e instanceof PlatformException && e.code === "unimplemented");
}

function isState(s: unknown): s is AppState {
  return s === "launching" || s === "running" || s === "suspended" || s === "terminating";
}

/** 应用(单例 {@link app})。 */
export class App extends EventEmitter<AppEvents> {
  static #instance: App | null = null;
  static get instance(): App {
    if (App.#instance === null) App.#instance = new App();
    return App.#instance;
  }

  readonly #channel = new MethodChannel("momo/app");
  #state: AppState = "launching";
  #active = false;
  #ready: Promise<void> | null = null;
  #resolveReady: (() => void) | null = null;

  private constructor() {
    super();
    this.#channel.setMethodCallHandler((call) => this.#handle(call));
  }

  /** 当前状态(宿主推送;启动期间为 "launching")。 */
  get state(): AppState {
    return this.#state;
  }
  /** 是否为前台应用。 */
  get isActive(): boolean {
    return this.#active;
  }
  /** 应用进入 running 时完成(已在运行则立即完成)。 */
  whenReady(): Promise<void> {
    if (this.#state === "running" || this.#state === "suspended") return Promise.resolve();
    this.#ready ??= new Promise<void>((resolve) => (this.#resolveReady = resolve));
    return this.#ready;
  }

  // ---- 方法(JS → 宿主) ----

  /** 请求退出(可被 'before-quit' / 窗口 'close' / 'will-quit' 取消)。 */
  quit(): void {
    void this.#invoke("quit");
  }
  /** 立即退出, 不触发可取消的事件;退出码 code。 */
  exit(code = 0): void {
    void this.#invoke("exit", { code });
  }
  /** 隐藏整个应用(全部窗口;应用进入 suspended)。 */
  hide(): void {
    void this.#invoke("hide");
  }
  show(): void {
    void this.#invoke("show");
  }
  /** 把应用带到前台。steal: 即使用户正在使用别的应用也抢占(否则只提示用户)。 */
  focus(options: { steal?: boolean } = {}): void {
    void this.#invoke("focus", { steal: options.steal ?? false });
  }
  /** 向宿主查询能力与状态。宿主不支持时为 null。 */
  async getCapabilities(): Promise<ReadonlySet<AppCapability> | null> {
    const r = await this.#invoke<Record<string, unknown>>("getState");
    if (r === null) return null;
    if (isState(r["state"])) this.#setState(r["state"]);
    this.#active = r["active"] === true;
    return new Set(Array.isArray(r["capabilities"]) ? (r["capabilities"] as AppCapability[]) : []);
  }

  /** 测试用: 清空状态与监听器。 */
  debugReset(): void {
    this.removeAllListeners();
    this.#state = "launching";
    this.#active = false;
    this.#ready = null;
    this.#resolveReady = null;
  }

  // ---- 宿主 → JS ----

  async #handle(call: MethodCall): Promise<unknown> {
    const args = (call.args ?? {}) as Record<string, unknown>;
    switch (call.method) {
      case "stateChanged":
        if (isState(args["state"])) this.#setState(args["state"]);
        return null;
      case "activeChanged": {
        const active = args["active"] === true;
        if (active === this.#active) return null;
        this.#active = active;
        this.emit(active ? "did-become-active" : "did-resign-active");
        return null;
      }
      case "beforeQuit":
        return this.#ask("before-quit");
      case "willQuit":
        return this.#ask("will-quit");
      case "windowAllClosed":
        this.emit("window-all-closed");
        return null;
      case "activate":
        this.emit("activate", { hasVisibleWindows: args["hasVisibleWindows"] === true });
        return null;
      default:
        return null;
    }
  }

  #setState(state: AppState): void {
    if (state === this.#state) return;
    this.#state = state;
    if (state === "running" || state === "suspended") {
      this.#resolveReady?.();
      this.#resolveReady = null;
    }
    this.emit("state-changed", state);
    if (state === "terminating") this.emit("quit");
  }

  async #ask(event: "before-quit" | "will-quit"): Promise<boolean> {
    const e = new HostEvent();
    this.emit(event, e);
    return e.settle();
  }

  async #invoke<R>(method: string, args?: Record<string, unknown>): Promise<R | null> {
    try {
      return await this.#channel.invokeMethod<R>(method, args ?? null);
    } catch (e) {
      if (!isUnsupported(e)) reportError(e, `calling momo/app ${method}`);
      return null;
    }
  }
}

/** 应用单例。 */
export const app: App = App.instance;
