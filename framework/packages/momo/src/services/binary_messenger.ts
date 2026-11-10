import * as services from "momo:services";
import { reportError } from "../foundation/assert";

/** 处理一条字节消息并(可选)回复。 */
export type BinaryMessageHandler = (data: ArrayBuffer | null) => ArrayBuffer | null | void | Promise<ArrayBuffer | null | void>;

/**
 * 字节消息收发器(对标 Flutter BinaryMessenger),封装 momo:services。
 * 测试钩子:{@link setMockMessageHandler} 拦截 JS→原生;{@link handlePlatformMessage} 模拟原生→JS。
 */
export class BinaryMessenger {
  #handlers = new Map<string, BinaryMessageHandler>();
  #mockHandlers = new Map<string, BinaryMessageHandler>();

  /** 发送到原生侧(或 mock handler);回复为原生 handler 的返回值, 无 handler 时为 null。 */
  send(channel: string, data: ArrayBuffer | null): Promise<ArrayBuffer | null> {
    const mock = this.#mockHandlers.get(channel);
    if (mock !== undefined) return this.#invoke(mock, data, channel);
    return services.send(channel, data);
  }

  /** 设置/清除接收原生消息的 handler。 */
  setMessageHandler(channel: string, handler: BinaryMessageHandler | null): void {
    if (handler === null) {
      this.#handlers.delete(channel);
      services.setHandler(channel, null);
      return;
    }
    this.#handlers.set(channel, handler);
    services.setHandler(channel, (data) => this.#invoke(handler, data, channel));
  }

  /** 是否已注册 handler。 */
  hasMessageHandler(channel: string): boolean {
    return this.#handlers.has(channel);
  }

  /** 测试用:拦截发往原生的消息(null 取消)。 */
  setMockMessageHandler(channel: string, handler: BinaryMessageHandler | null): void {
    if (handler === null) this.#mockHandlers.delete(channel);
    else this.#mockHandlers.set(channel, handler);
  }

  /** 模拟一条原生 → JS 消息, 返回 JS handler 的回复(无 handler 时 null)。 */
  handlePlatformMessage(channel: string, data: ArrayBuffer | null): Promise<ArrayBuffer | null> {
    const handler = this.#handlers.get(channel);
    if (handler === undefined) return Promise.resolve(null);
    return this.#invoke(handler, data, channel);
  }

  /** 清除全部 mock handler。 */
  debugReset(): void {
    this.#mockHandlers.clear();
  }

  async #invoke(handler: BinaryMessageHandler, data: ArrayBuffer | null, channel: string): Promise<ArrayBuffer | null> {
    try {
      const r = await handler(data);
      return r ?? null;
    } catch (e) {
      reportError(e, `while handling a message on channel "${channel}"`);
      return null;
    }
  }
}

/** 默认消息收发器。 */
export const defaultBinaryMessenger = new BinaryMessenger();
