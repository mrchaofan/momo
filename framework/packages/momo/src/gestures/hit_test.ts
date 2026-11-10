import { Offset } from "../painting/geometry";
import { Matrix4 } from "../painting/matrix4";
import type { PointerEvent } from "./events";

/** 可接收命中事件的对象。 */
export interface HitTestTarget {
  handleEvent(event: PointerEvent, entry: HitTestEntry): void;
}

/** 命中路径上的一项。 */
export class HitTestEntry {
  /** 全局 → 该目标局部坐标的变换(由 HitTestResult.add 填充)。 */
  transform: Matrix4 | null = null;
  constructor(readonly target: HitTestTarget) {}
}

/**
 * 命中测试结果:路径从最深(最先添加)到根。维护全局 → 局部的变换栈。
 */
export class HitTestResult {
  readonly path: HitTestEntry[] = [];
  #transforms: Matrix4[] = [];

  /** 添加命中项(记录当前变换)。 */
  add(entry: HitTestEntry): void {
    entry.transform = this.#transforms.length > 0 ? this.#transforms[this.#transforms.length - 1]! : null;
    this.path.push(entry);
  }

  /** 压入 parent → child 的坐标变换。 */
  pushTransform(transform: Matrix4): void {
    const top = this.#transforms.length > 0 ? this.#transforms[this.#transforms.length - 1]! : null;
    this.#transforms.push(top === null ? transform.clone() : transform.clone().multiply(top));
  }

  /** 压入平移(局部 = 父坐标 + offset)。 */
  pushOffset(offset: Offset): void {
    this.pushTransform(Matrix4.translationValues(offset.dx, offset.dy));
  }

  popTransform(): void {
    this.#transforms.pop();
  }

  /**
   * 子以 offset 摆放在父中时, 以子坐标测试:position - offset。
   */
  addWithPaintOffset(offset: Offset | null, position: Offset, hitTest: (result: HitTestResult, position: Offset) => boolean): boolean {
    if (offset === null || (offset.dx === 0 && offset.dy === 0)) return hitTest(this, position);
    this.pushOffset(offset.neg());
    const hit = hitTest(this, position.minus(offset));
    this.popTransform();
    return hit;
  }

  /**
   * 子到父的绘制变换为 transform 时, 以子坐标测试(变换不可逆则视为未命中)。
   */
  addWithPaintTransform(transform: Matrix4 | null, position: Offset, hitTest: (result: HitTestResult, position: Offset) => boolean): boolean {
    if (transform === null) return hitTest(this, position);
    const inverse = transform.inverted();
    if (inverse === null) return false;
    this.pushTransform(inverse);
    const hit = hitTest(this, inverse.transformPoint(position));
    this.popTransform();
    return hit;
  }
}
