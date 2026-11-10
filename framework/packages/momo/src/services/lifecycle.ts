import { reportError } from "../foundation/assert";
import { defaultBinaryMessenger } from "./binary_messenger";
import { utf8Decode } from "./message_codecs";
import { StandardMessageCodec } from "./standard_codec";

/** 应用生命周期状态。 */
export type AppLifecycleState = "resumed" | "suspended" | "terminating";

/** 生命周期监听。 */
export type AppLifecycleListener = (state: AppLifecycleState) => void;

function isState(s: unknown): s is AppLifecycleState {
  return s === "resumed" || s === "suspended" || s === "terminating";
}

/** 应用生命周期(momo/lifecycle 通道, 载荷为 StandardCodec 字符串;也宽容接受原始 UTF-8)。 */
export class AppLifecycle {
  static #instance: AppLifecycle | null = null;
  static get instance(): AppLifecycle {
    if (AppLifecycle.#instance === null) AppLifecycle.#instance = new AppLifecycle();
    return AppLifecycle.#instance;
  }

  #state: AppLifecycleState | null = null;
  #listeners: AppLifecycleListener[] = [];
  #initialized = false;

  private constructor() {}

  /** 最近一次收到的状态(尚未收到为 null)。 */
  get state(): AppLifecycleState | null {
    return this.#state;
  }

  addListener(listener: AppLifecycleListener): void {
    this.#listeners.push(listener);
  }

  removeListener(listener: AppLifecycleListener): void {
    const i = this.#listeners.indexOf(listener);
    if (i >= 0) this.#listeners.splice(i, 1);
  }

  /** 注册通道 handler(幂等)。 */
  ensureInitialized(): void {
    if (this.#initialized) return;
    this.#initialized = true;
    defaultBinaryMessenger.setMessageHandler("momo/lifecycle", (data) => {
      this.#handle(data);
      return null;
    });
  }

  #handle(data: ArrayBuffer | null): void {
    if (data === null || data.byteLength === 0) return;
    const bytes = new Uint8Array(data);
    let value: unknown;
    if (bytes[0] === 7) {
      try {
        value = StandardMessageCodec.instance.decodeMessage(data);
      } catch {
        value = utf8Decode(bytes);
      }
    } else {
      value = utf8Decode(bytes);
    }
    if (!isState(value)) {
      reportError(new Error(`unknown lifecycle state: ${String(value)}`), "in momo/lifecycle");
      return;
    }
    this.#state = value;
    for (const l of this.#listeners.slice()) {
      try {
        l(value);
      } catch (e) {
        reportError(e, "in an AppLifecycle listener");
      }
    }
  }

  /** 测试用:清空状态与监听者(handler 保留)。 */
  debugReset(): void {
    this.#state = null;
    this.#listeners = [];
  }
}
