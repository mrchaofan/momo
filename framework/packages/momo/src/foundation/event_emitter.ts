// 带类型的事件发射器(对标 Node EventEmitter / Electron 的 app、BrowserWindow 事件)。框架自己实现, 不依赖
// node:events: 同一份代码在 momo 引擎(Node 环境)、headless 与 Node mock 下行为一致。
// 监听器抛出的异常经 reportError 报告, 不影响其他监听器。
import { reportError } from "./assert";

/** 事件表: 事件名 → 监听器参数元组。 */
export type EventMap = { [event: string]: unknown[] };

// 监听器以不透明函数保存;类型由 on/off/emit 的签名保证。
type AnyListener = (...args: unknown[]) => void;
type Entry = { readonly fn: AnyListener; readonly once: boolean };

export class EventEmitter<Events extends EventMap> {
  #listeners = new Map<keyof Events, Entry[]>();

  on<K extends keyof Events>(event: K, listener: (...args: Events[K]) => void): this {
    return this.#add(event, listener, false);
  }
  once<K extends keyof Events>(event: K, listener: (...args: Events[K]) => void): this {
    return this.#add(event, listener, true);
  }
  off<K extends keyof Events>(event: K, listener: (...args: Events[K]) => void): this {
    const list = this.#listeners.get(event);
    if (list === undefined) return this;
    const i = list.findIndex((e) => e.fn === (listener as unknown as AnyListener));
    if (i >= 0) list.splice(i, 1);
    if (list.length === 0) this.#listeners.delete(event);
    return this;
  }
  listenerCount<K extends keyof Events>(event: K): number {
    return this.#listeners.get(event)?.length ?? 0;
  }
  /** 移除 event 的全部监听器(不给 event 时移除所有事件的)。 */
  removeAllListeners<K extends keyof Events>(event?: K): this {
    if (event === undefined) this.#listeners.clear();
    else this.#listeners.delete(event);
    return this;
  }

  /** 按注册顺序同步调用监听器;返回是否有监听器。 */
  protected emit<K extends keyof Events>(event: K, ...args: Events[K]): boolean {
    const list = this.#listeners.get(event);
    if (list === undefined || list.length === 0) return false;
    for (const entry of [...list]) {
      if (entry.once) this.off(event, entry.fn as unknown as (...a: Events[K]) => void);
      try {
        entry.fn(...args);
      } catch (e) {
        reportError(e, `in a '${String(event)}' listener`);
      }
    }
    return true;
  }

  #add<K extends keyof Events>(event: K, listener: (...args: Events[K]) => void, once: boolean): this {
    let list = this.#listeners.get(event);
    if (list === undefined) this.#listeners.set(event, (list = []));
    list.push({ fn: listener as unknown as AnyListener, once });
    return this;
  }
}

/**
 * 宿主发来的可取消事件(app 'before-quit' / 'will-quit'、Widget 'close')。监听器同步调用 preventDefault() 取消;
 * 需要异步决定时用 waitUntil(promise): 宿主等 promise 完成后再继续, promise 结果为 false(或 reject)视为取消。
 * 宿主对每次询问有超时(界面线程卡死时强制退出), waitUntil 不应无限等待。
 */
export class HostEvent {
  #prevented = false;
  readonly #waits: Promise<unknown>[] = [];

  preventDefault(): void {
    this.#prevented = true;
  }
  get defaultPrevented(): boolean {
    return this.#prevented;
  }
  waitUntil(promise: Promise<unknown>): void {
    this.#waits.push(promise);
  }

  /** @internal 全部监听器调用之后: 等待 waitUntil, 返回是否放行。 */
  async settle(): Promise<boolean> {
    for (const p of this.#waits) {
      try {
        if ((await p) === false) this.#prevented = true;
      } catch (e) {
        reportError(e, "in a HostEvent.waitUntil promise");
        this.#prevented = true;
      }
    }
    return !this.#prevented;
  }
}
