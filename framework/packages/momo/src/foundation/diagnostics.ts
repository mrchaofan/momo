const ids = new WeakMap<object, number>();
let nextId = 1;

/** 对象的短哈希(调试显示用, 进程内稳定)。 */
export function shortHash(object: object): string {
  let id = ids.get(object);
  if (id === undefined) {
    id = nextId++;
    ids.set(object, id);
  }
  return id.toString(16).padStart(5, "0");
}

/** "类名#hash" 形式的对象标识。 */
export function describeIdentity(object: object): string {
  return `${object.constructor.name}#${shortHash(object)}`;
}

/** 调试打印(经引擎注入的 console)。 */
export function debugPrint(message: string): void {
  console.log(message);
}

/** 可诊断对象:提供树形描述。 */
export interface Diagnosticable {
  toStringShort(): string;
}

/** 将树形结构渲染为缩进文本。 */
export function describeTree<T>(root: T, label: (node: T) => string, children: (node: T) => readonly T[]): string {
  const lines: string[] = [];
  const walk = (node: T, prefix: string, childPrefix: string): void => {
    lines.push(prefix + label(node));
    const kids = children(node);
    for (let i = 0; i < kids.length; i++) {
      const last = i === kids.length - 1;
      walk(kids[i]!, childPrefix + (last ? "└─" : "├─"), childPrefix + (last ? "  " : "│ "));
    }
  };
  walk(root, "", "");
  return lines.join("\n");
}
