import type { VoidCallback } from "../foundation/change_notifier";
import {
  Animation,
  AnimationWithLocalListeners,
  AnimationWithParent,
  type AnimationStatus,
  type AnimationStatusListener,
} from "./animation";
import type { Curve } from "./curves";

/** 永远停在给定值的动画(监听无效)。 */
export class AlwaysStoppedAnimation<T> extends Animation<T> {
  readonly #value: T;
  readonly #status: AnimationStatus;
  constructor(value: T, status: AnimationStatus = "forward") {
    super();
    this.#value = value;
    this.#status = status;
  }
  get value(): T {
    return this.#value;
  }
  get status(): AnimationStatus {
    return this.#status;
  }
  addListener(_listener: VoidCallback): void {}
  removeListener(_listener: VoidCallback): void {}
  addStatusListener(_listener: AnimationStatusListener): void {}
  removeStatusListener(_listener: AnimationStatusListener): void {}
}

/** 值恒为 1、状态 completed 的动画。 */
export const kAlwaysCompleteAnimation: Animation<number> = new AlwaysStoppedAnimation(1, "completed");
/** 值恒为 0、状态 dismissed 的动画。 */
export const kAlwaysDismissedAnimation: Animation<number> = new AlwaysStoppedAnimation(0, "dismissed");

function reverseStatus(status: AnimationStatus): AnimationStatus {
  switch (status) {
    case "forward":
      return "reverse";
    case "reverse":
      return "forward";
    case "completed":
      return "dismissed";
    case "dismissed":
      return "completed";
  }
}

/** 反向动画:value = 1 - parent.value, 状态相应翻转。 */
export class ReverseAnimation extends AnimationWithLocalListeners<number> {
  #subscribed = false;
  constructor(readonly parent: Animation<number>) {
    super();
  }
  get value(): number {
    return 1 - this.parent.value;
  }
  get status(): AnimationStatus {
    return reverseStatus(this.parent.status);
  }
  #onParentValue = (): void => this.notifyListeners();
  #onParentStatus = (status: AnimationStatus): void => this.notifyStatusListeners(reverseStatus(status));
  protected didRegisterListener(): void {
    if (this.#subscribed) return;
    this.#subscribed = true;
    this.parent.addListener(this.#onParentValue);
    this.parent.addStatusListener(this.#onParentStatus);
  }
  protected didUnregisterListener(): void {
    if (!this.#subscribed || this.listenerCount > 0) return;
    this.#subscribed = false;
    this.parent.removeListener(this.#onParentValue);
    this.parent.removeStatusListener(this.#onParentStatus);
  }
}

/**
 * 对父动画施加曲线。父动画反向运行且提供了 reverseCurve 时使用 reverseCurve;
 * 方向在一次运行中保持不变(到达 completed/dismissed 时重置), 语义同 Flutter。
 */
export class CurvedAnimation extends AnimationWithParent<number> {
  curve: Curve;
  reverseCurve: Curve | null;
  #curveDirection: AnimationStatus | null = null;

  constructor(opts: { parent: Animation<number>; curve: Curve; reverseCurve?: Curve | null }) {
    super(opts.parent);
    this.curve = opts.curve;
    this.reverseCurve = opts.reverseCurve ?? null;
    this.#updateCurveDirection(opts.parent.status);
    opts.parent.addStatusListener(this.#updateCurveDirection);
  }

  #updateCurveDirection = (status: AnimationStatus): void => {
    switch (status) {
      case "dismissed":
      case "completed":
        this.#curveDirection = null;
        break;
      case "forward":
        this.#curveDirection ??= "forward";
        break;
      case "reverse":
        this.#curveDirection ??= "reverse";
        break;
    }
  };

  get #useForwardCurve(): boolean {
    return this.reverseCurve === null || (this.#curveDirection ?? this.parent.status) !== "reverse";
  }

  get value(): number {
    const active = this.#useForwardCurve ? this.curve : this.reverseCurve;
    const t = this.parent.value;
    if (active === null) return t;
    if (t === 0 || t === 1) return t;
    return active.transform(t);
  }

  /** 解除对父动画的状态监听。 */
  dispose(): void {
    this.parent.removeStatusListener(this.#updateCurveDirection);
  }
}

/** 可替换父动画的代理;parent 为 null 时停在最后的值与状态。 */
export class ProxyAnimation extends AnimationWithLocalListeners<number> {
  #parent: Animation<number> | null;
  #value = 0;
  #status: AnimationStatus = "dismissed";
  #subscribed = false;

  constructor(animation?: Animation<number> | null) {
    super();
    this.#parent = animation ?? null;
    if (this.#parent === null) {
      this.#value = 0;
      this.#status = "dismissed";
    }
  }

  get parent(): Animation<number> | null {
    return this.#parent;
  }
  set parent(value: Animation<number> | null) {
    if (value === this.#parent) return;
    const old = this.#parent;
    if (old !== null) {
      this.#value = old.value;
      this.#status = old.status;
      if (this.#subscribed) this.#unsubscribe(old);
    }
    this.#parent = value;
    if (value !== null) {
      if (this.#subscribed) this.#subscribe(value);
      if (this.#value !== value.value) this.notifyListeners();
      if (this.#status !== value.status) this.notifyStatusListeners(value.status);
    }
  }

  get value(): number {
    return this.#parent !== null ? this.#parent.value : this.#value;
  }
  get status(): AnimationStatus {
    return this.#parent !== null ? this.#parent.status : this.#status;
  }

  #onValue = (): void => this.notifyListeners();
  #onStatus = (s: AnimationStatus): void => this.notifyStatusListeners(s);
  #subscribe(p: Animation<number>): void {
    p.addListener(this.#onValue);
    p.addStatusListener(this.#onStatus);
  }
  #unsubscribe(p: Animation<number>): void {
    p.removeListener(this.#onValue);
    p.removeStatusListener(this.#onStatus);
  }
  protected didRegisterListener(): void {
    if (this.#subscribed) return;
    this.#subscribed = true;
    if (this.#parent !== null) this.#subscribe(this.#parent);
  }
  protected didUnregisterListener(): void {
    if (!this.#subscribed || this.listenerCount > 0) return;
    this.#subscribed = false;
    if (this.#parent !== null) this.#unsubscribe(this.#parent);
  }
}
