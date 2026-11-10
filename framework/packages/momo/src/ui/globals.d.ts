// 引擎全局对象的最小声明(非 DOM / 非 Node)。
// 引擎在 V8 上下文里注入了 console(见 examples/hello_frame);框架除此之外不依赖任何宿主全局。
declare global {
  interface MomoConsole {
    log(...args: unknown[]): void;
    info(...args: unknown[]): void;
    warn(...args: unknown[]): void;
    error(...args: unknown[]): void;
    debug(...args: unknown[]): void;
  }
  // eslint-disable-next-line no-var
  var console: MomoConsole;
}
export {};
