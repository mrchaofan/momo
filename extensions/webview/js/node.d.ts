// 本扩展用到的 Node API 的类型(运行时是引擎内嵌的 Node;工程不依赖 @types/node, 这里只声明用到的部分)。
declare module "node:fs/promises" {
  export function readFile(path: string): Promise<Uint8Array>;
}

declare module "node:path" {
  const path: {
    readonly sep: string;
    resolve(...segments: string[]): string;
    extname(path: string): string;
  };
  export default path;
}

declare class URL {
  constructor(url: string);
  readonly pathname: string;
}
