import { reportError } from "./assert";

/** 无参回调。 */
export type VoidCallback = () => void;

/** 可监听对象。 */
export interface Listenable {
  addListener(listener: VoidCallback): void;
  removeListener(listener: VoidCallback): void;
}

/** 持有当前值的可监听对象。 */
export interface ValueListenable<T> extends Listenable {
  readonly value: T;
}

/**
 * 变化通知器:维护监听者列表, notifyListeners 时依次回调。
 * 回调期间增删监听者是安全的(删除立即生效, 新增从下次通知起生效)。
 */
export class ChangeNotifier implements Listenable {
  #listeners: (VoidCallback | null)[] = [];
  #notifyDepth = 0;
  #pendingRemovals = 0;
  #disposed = false;

  /** 是否有监听者。 */
  get hasListeners(): boolean {
    return this.#listeners.length - this.#pendingRemovals > 0;
  }

  addListener(listener: VoidCallback): void {
    if (this.#disposed) throw new Error(`${this.constructor.name} was used after being disposed.`);
    this.#listeners.push(listener);
  }

  removeListener(listener: VoidCallback): void {
    const list = this.#listeners;
    for (let i = 0; i < list.length; i++) {
      if (list[i] === listener) {
        if (this.#notifyDepth > 0) {
          list[i] = null;
          this.#pendingRemovals++;
        } else {
          list.splice(i, 1);
        }
        return;
      }
    }
  }

  /** 释放;之后不能再添加监听者。 */
  dispose(): void {
    this.#listeners = [];
    this.#pendingRemovals = 0;
    this.#disposed = true;
  }

  /** 通知所有监听者。 */
  notifyListeners(): void {
    const list = this.#listeners;
    const end = list.length;
    if (end === 0) return;
    this.#notifyDepth++;
    for (let i = 0; i < end; i++) {
      const listener = list[i];
      if (listener === null || listener === undefined) continue;
      try {
        listener();
      } catch (e) {
        reportError(e, `while dispatching notifications for ${this.constructor.name}`);
      }
    }
    this.#notifyDepth--;
    if (this.#notifyDepth === 0 && this.#pendingRemovals > 0) {
      this.#listeners = list.filter((l) => l !== null);
      this.#pendingRemovals = 0;
    }
  }
}

/** 持有单个值的 ChangeNotifier;值变化(!==)时通知。 */
export class ValueNotifier<T> extends ChangeNotifier implements ValueListenable<T> {
  #value: T;
  constructor(value: T) {
    super();
    this.#value = value;
  }
  get value(): T {
    return this.#value;
  }
  set value(newValue: T) {
    if (this.#value === newValue) return;
    this.#value = newValue;
    this.notifyListeners();
  }
}

/** 合并多个 Listenable:任意一个通知即回调。 */
export function mergeListenables(listenables: readonly (Listenable | null | undefined)[]): Listenable {
  const items = listenables.filter((l): l is Listenable => l != null);
  return {
    addListener(listener: VoidCallback): void {
      for (const l of items) l.addListener(listener);
    },
    removeListener(listener: VoidCallback): void {
      for (const l of items) l.removeListener(listener);
    },
  };
}
