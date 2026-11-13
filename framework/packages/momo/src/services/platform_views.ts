import { reportError } from "../foundation/assert";
import { MethodChannel, type MethodCall } from "./platform_channel";

/**
 * 平台视图服务(docs/05 §9): 经 momo/platform_views 通道创建/销毁/聚焦由扩展提供的系统原生视图。
 * 一般通过 {@link PlatformView} view 使用;需要自管生命周期时用 {@link PlatformViewsService.create}。
 *
 * 视图 id 由 JS 分配: 高位是每个 JS 运行时随机的标签, 低 20 位递增。热重启后旧运行时的在途消息
 * 不会误伤新运行时的视图(引擎在热重启时销毁全部旧视图)。
 */
const channel = new MethodChannel("momo/platform_views");

const kCounterBits = 2 ** 20;
const runtimeTag = 1 + Math.floor(Math.random() * (2 ** 30 - 1));
let nextCounter = 1;

type FocusListener = () => void;
const focusListeners = new Map<number, FocusListener>();
let handlerInstalled = false;

function ensureHandler(): void {
  if (handlerInstalled) return;
  handlerInstalled = true;
  channel.setMethodCallHandler((call: MethodCall) => {
    if (call.method === "focused") {
      const id = idOf(call.args);
      if (id !== null) focusListeners.get(id)?.();
    }
    return null;
  });
}

function idOf(args: unknown): number | null {
  let v: unknown;
  if (args instanceof Map) v = args.get("id");
  else if (args !== null && typeof args === "object") v = (args as Record<string, unknown>).id;
  if (typeof v === "bigint") v = Number(v);
  return typeof v === "number" && Number.isSafeInteger(v) ? v : null;
}

/** 一个已创建的平台视图。 */
export class PlatformViewController {
  #disposed = false;
  /** @internal */
  constructor(readonly viewId: number, readonly viewType: string) {}

  get disposed(): boolean {
    return this.#disposed;
  }

  /** 把键盘焦点交给原生视图。 */
  async focus(): Promise<void> {
    if (this.#disposed) return;
    await channel.invokeMethod("focus", { id: this.viewId });
  }

  /** 原生视图因用户操作获得键盘焦点时回调(null 取消)。 */
  setOnFocused(listener: FocusListener | null): void {
    if (this.#disposed) return;
    if (listener === null) {
      focusListeners.delete(this.viewId);
      return;
    }
    ensureHandler();
    focusListeners.set(this.viewId, listener);
  }

  /** 销毁原生视图。幂等。 */
  async dispose(): Promise<void> {
    if (this.#disposed) return;
    this.#disposed = true;
    focusListeners.delete(this.viewId);
    await channel.invokeMethod("dispose", { id: this.viewId });
  }
}

export const PlatformViewsService = {
  /** 分配一个新的视图 id(尚未创建)。 */
  allocateId(): number {
    const counter = ((nextCounter++ - 1) % (kCounterBits - 1)) + 1;  // 1 .. 2^20-1
    return runtimeTag * kCounterBits + counter;
  },

  /**
   * 创建视图类型为 viewType 的平台视图(类型由扩展注册, 如 "momo.webview")。
   * params 为传给扩展工厂的创建参数(字节)。失败时抛 PlatformException(如类型未注册)。
   */
  async create(viewType: string, params: Uint8Array | null = null, id: number = PlatformViewsService.allocateId()): Promise<PlatformViewController> {
    await channel.invokeMethod("create", { id, viewType, params });
    return new PlatformViewController(id, viewType);
  },

  /** @internal 测试用: 模拟原生发来的 focused 事件。 */
  debugNotifyFocused(id: number): void {
    const listener = focusListeners.get(id);
    if (listener !== undefined) listener();
  },
};

/** 把 dispose 等"发出即忘"的调用错误报告出来。 */
export function reportPlatformViewError(e: unknown, what: string): void {
  reportError(e, `in momo/platform_views ${what}`);
}
