// momo:services 的 mock。原生侧 handler 可由测试经 __mockServices 注册;无 handler 时回复 null。
export type MessageHandler =
  (data: ArrayBuffer | null) => ArrayBuffer | null | void | Promise<ArrayBuffer | null | void>;

const jsHandlers = new Map<string, MessageHandler>();
const nativeHandlers = new Map<string, (data: ArrayBuffer | null) => ArrayBuffer | null | Promise<ArrayBuffer | null>>();
const sent: { channel: string; data: ArrayBuffer | null }[] = [];

// 到达 JS 的顺序与真实引擎一致: 宿主的回复与通知是 ui 线程上同一个队列里的任务, 一个处理完(连同它引出的
// microtask)才轮到下一个。原生 handler 在调用时同步执行(宿主状态立即更新), 只有它的回复排队。
let arrivals: Promise<unknown> = Promise.resolve();

async function settle(): Promise<void> {
  for (let i = 0; i < 4; i++) await Promise.resolve();
}

function arrive<T>(run: () => T | Promise<T>): Promise<T> {
  const result = arrivals.then(run);
  arrivals = result.then(settle, settle);
  return result;
}

export function send(channel: string, data: ArrayBuffer | null): Promise<ArrayBuffer | null> {
  sent.push({ channel, data });
  const h = nativeHandlers.get(channel);
  const reply = h ? h(data) : null;
  return arrive(() => reply);
}

export function setHandler(channel: string, handler: MessageHandler | null): void {
  if (handler === null) jsHandlers.delete(channel);
  else jsHandlers.set(channel, handler);
}

export const isDev = true;
export const platform = "headless";
export const assetsDir = "/mock/assets";
export const nativeHost = 0n;

export const __mockServices = {
  sent,
  setNativeHandler(channel: string, h: ((data: ArrayBuffer | null) => ArrayBuffer | null | Promise<ArrayBuffer | null>) | null): void {
    if (h === null) nativeHandlers.delete(channel);
    else nativeHandlers.set(channel, h);
  },
  /** 模拟原生 → JS 消息。 */
  deliver(channel: string, data: ArrayBuffer | null): Promise<ArrayBuffer | null> {
    return arrive(async () => {
      const h = jsHandlers.get(channel);
      if (!h) return null;
      const r = await h(data);
      return r ?? null;
    });
  },
  /** 已排队的回复与通知全部到达并处理完。 */
  async idle(): Promise<void> {
    let seen: Promise<unknown> | null = null;
    while (seen !== arrivals) {
      seen = arrivals;
      await seen;
    }
  },
  hasJsHandler(channel: string): boolean {
    return jsHandlers.has(channel);
  },
};
