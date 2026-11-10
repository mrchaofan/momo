import type { ValueListenable, VoidCallback } from "../foundation/change_notifier";
import { reportError } from "../foundation/assert";
import type { Animatable } from "./tween";

/** 动画状态。 */
export type AnimationStatus = "dismissed" | "forward" | "reverse" | "completed";

/** 状态监听回调。 */
export type AnimationStatusListener = (status: AnimationStatus) => void;

/**
 * 随时间变化的值(对标 Flutter Animation<T>)。value 变化时通知监听者, status 变化时通知状态监听者。
 */
export abstract class Animation<T> implements ValueListenable<T> {
  abstract get value(): T;
  abstract get status(): AnimationStatus;
  abstract addListener(listener: VoidCallback): void;
  abstract removeListener(listener: VoidCallback): void;
  abstract addStatusListener(listener: AnimationStatusListener): void;
  abstract removeStatusListener(listener: AnimationStatusListener): void;

  /** 是否正在向前或向后运行。 */
  get isAnimating(): boolean {
    const s = this.status;
    return s === "forward" || s === "reverse";
  }
  /** 是否停在终点。 */
  get isCompleted(): boolean {
    return this.status === "completed";
  }
  /** 是否停在起点。 */
  get isDismissed(): boolean {
    return this.status === "dismissed";
  }
  /** 用 animatable 变换本动画:等价于 animatable.animate(this)。 */
  drive<U>(animatable: Animatable<U>): Animation<U> {
    return animatable.animate(this as unknown as Animation<number>);
  }
  toString(): string {
    return `${this.constructor.name}(${String(this.value)}; ${this.status})`;
  }
}

/**
 * 写时复制的监听者列表:notify 期间零分配, add/remove(低频)时替换数组,
 * 因此回调中增删监听者是安全的。
 */
export class ListenerList<F extends (...args: never[]) => void> {
  #list: F[] = [];
  get length(): number {
    return this.#list.length;
  }
  add(f: F): void {
    const next = this.#list.slice();
    next.push(f);
    this.#list = next;
  }
  remove(f: F): boolean {
    const i = this.#list.indexOf(f);
    if (i < 0) return false;
    const next = this.#list.slice();
    next.splice(i, 1);
    this.#list = next;
    return true;
  }
  clear(): void {
    this.#list = [];
  }
  /** 当前快照(调用期间不会被修改)。 */
  get snapshot(): readonly F[] {
    return this.#list;
  }
}

/**
 * 自带本地监听者列表的 Animation 基类。子类调用 notifyListeners/notifyStatusListeners;
 * 可覆盖 didRegisterListener/didUnregisterListener 实现惰性订阅父动画。
 */
export abstract class AnimationWithLocalListeners<T> extends Animation<T> {
  #listeners = new ListenerList<VoidCallback>();
  #statusListeners = new ListenerList<AnimationStatusListener>();

  addListener(listener: VoidCallback): void {
    this.#listeners.add(listener);
    this.didRegisterListener();
  }
  removeListener(listener: VoidCallback): void {
    if (this.#listeners.remove(listener)) this.didUnregisterListener();
  }
  addStatusListener(listener: AnimationStatusListener): void {
    this.#statusListeners.add(listener);
    this.didRegisterListener();
  }
  removeStatusListener(listener: AnimationStatusListener): void {
    if (this.#statusListeners.remove(listener)) this.didUnregisterListener();
  }
  /** 当前监听者总数(含状态监听者)。 */
  protected get listenerCount(): number {
    return this.#listeners.length + this.#statusListeners.length;
  }
  /** 登记了监听者(子类可用于惰性订阅)。 */
  protected didRegisterListener(): void {}
  /** 移除了监听者。 */
  protected didUnregisterListener(): void {}
  /** 清空所有监听者。 */
  protected clearListeners(): void {
    this.#listeners.clear();
    this.#statusListeners.clear();
  }
  /** 通知值监听者。 */
  notifyListeners(): void {
    const list = this.#listeners.snapshot;
    for (let i = 0; i < list.length; i++) {
      try {
        list[i]!();
      } catch (e) {
        reportError(e, `while notifying listeners for ${this.constructor.name}`);
      }
    }
  }
  /** 通知状态监听者。 */
  notifyStatusListeners(status: AnimationStatus): void {
    const list = this.#statusListeners.snapshot;
    for (let i = 0; i < list.length; i++) {
      try {
        list[i]!(status);
      } catch (e) {
        reportError(e, `while notifying status listeners for ${this.constructor.name}`);
      }
    }
  }
}

/** 监听者直接转发到父动画的 Animation 基类(值由父动画派生)。 */
export abstract class AnimationWithParent<T, P = number> extends Animation<T> {
  constructor(readonly parent: Animation<P>) {
    super();
  }
  get status(): AnimationStatus {
    return this.parent.status;
  }
  addListener(listener: VoidCallback): void {
    this.parent.addListener(listener);
  }
  removeListener(listener: VoidCallback): void {
    this.parent.removeListener(listener);
  }
  addStatusListener(listener: AnimationStatusListener): void {
    this.parent.addStatusListener(listener);
  }
  removeStatusListener(listener: AnimationStatusListener): void {
    this.parent.removeStatusListener(listener);
  }
}
