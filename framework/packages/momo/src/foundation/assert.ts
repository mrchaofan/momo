/** 框架错误类型(对标 FlutterError)。 */
export class MomoError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "MomoError";
  }
}

/** 断言:条件不成立时抛出 {@link MomoError}。框架内部用于协议检查。 */
export function assert(condition: unknown, message?: string | (() => string)): asserts condition {
  if (!condition) {
    const text = typeof message === "function" ? message() : message;
    throw new MomoError(text ?? "Assertion failed");
  }
}

/** 调试模式开关(可由应用/测试修改);关闭后部分昂贵的调试检查会跳过。 */
export const debugFlags = {
  /** 是否执行昂贵的调试断言。 */
  checks: true,
  /** 打印每帧的阶段耗时(调试用)。 */
  printFrameTimings: false,
};

/** 报告框架内部捕获的异常(回调/通道处理器里抛出的错误)。可替换以接入崩溃上报。 */
export let reportError: (error: unknown, context: string) => void = (error, context) => {
  const text = error instanceof Error ? `${error.message}\n${error.stack ?? ""}` : String(error);
  console.error(`[momo] Exception ${context}: ${text}`);
};

/** 替换全局错误报告器(返回旧的)。 */
export function setErrorReporter(reporter: (error: unknown, context: string) => void): (error: unknown, context: string) => void {
  const old = reportError;
  reportError = reporter;
  return old;
}
