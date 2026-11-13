import { shortHash } from "./diagnostics";

/**
 * View 身份标识。canUpdate 判定 = 构造函数相同 且 key 相等({@link Key.equals})。
 */
export abstract class Key {
  /** 值语义相等。 */
  abstract equals(other: Key): boolean;
}

/** 在兄弟节点间唯一的 key。 */
export abstract class LocalKey extends Key {}

/** 以值(===,数字 NaN 视为相等)判等的 key。 */
export class ValueKey<T = unknown> extends LocalKey {
  constructor(readonly value: T) {
    super();
  }
  equals(other: Key): boolean {
    return other instanceof ValueKey && other.constructor === this.constructor && Object.is(other.value, this.value);
  }
  toString(): string {
    return `[ValueKey ${String(this.value)}]`;
  }
}

/** 以对象身份判等的 key。 */
export class ObjectKey extends LocalKey {
  constructor(readonly value: object | null) {
    super();
  }
  equals(other: Key): boolean {
    return other instanceof ObjectKey && other.value === this.value;
  }
}

/** 只与自身相等的 key。 */
export class UniqueKey extends LocalKey {
  equals(other: Key): boolean {
    return other === this;
  }
  toString(): string {
    return `[#${shortHash(this)}]`;
  }
}

/** 两个可空 key 是否相等。 */
export function keysEqual(a: Key | null | undefined, b: Key | null | undefined): boolean {
  if (a == null || b == null) return a == null && b == null;
  return a === b || a.equals(b);
}

/** key 的哈希桶值(用于 diff 时按值查找):ValueKey/ObjectKey 取其 value, 其余取自身。 */
export function keyHashOf(key: Key): unknown {
  if (key instanceof ValueKey) return key.value;
  if (key instanceof ObjectKey) return key.value;
  return key;
}
