// momo:services 的 mock。原生侧 handler 可由测试经 __mockServices 注册;无 handler 时回复 null。
export type MessageHandler =
  (data: ArrayBuffer | null) => ArrayBuffer | null | void | Promise<ArrayBuffer | null | void>;

const jsHandlers = new Map<string, MessageHandler>();
const nativeHandlers = new Map<string, (data: ArrayBuffer | null) => ArrayBuffer | null | Promise<ArrayBuffer | null>>();
const sent: { channel: string; data: ArrayBuffer | null }[] = [];

export function send(channel: string, data: ArrayBuffer | null): Promise<ArrayBuffer | null> {
  sent.push({ channel, data });
  const h = nativeHandlers.get(channel);
  if (!h) return Promise.resolve(null);
  return Promise.resolve(h(data));
}

export function setHandler(channel: string, handler: MessageHandler | null): void {
  if (handler === null) jsHandlers.delete(channel);
  else jsHandlers.set(channel, handler);
}

export const isDev = true;
export const platform = "headless";

export const __mockServices = {
  sent,
  setNativeHandler(channel: string, h: ((data: ArrayBuffer | null) => ArrayBuffer | null | Promise<ArrayBuffer | null>) | null): void {
    if (h === null) nativeHandlers.delete(channel);
    else nativeHandlers.set(channel, h);
  },
  /** 模拟原生 → JS 消息。 */
  async deliver(channel: string, data: ArrayBuffer | null): Promise<ArrayBuffer | null> {
    const h = jsHandlers.get(channel);
    if (!h) return null;
    const r = await h(data);
    return r ?? null;
  },
  hasJsHandler(channel: string): boolean {
    return jsHandlers.has(channel);
  },
};
