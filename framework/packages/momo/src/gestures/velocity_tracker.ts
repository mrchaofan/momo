import { Offset } from "../painting/geometry";
import { kMaxFlingVelocity } from "./constants";

const kHistorySize = 20;
const kHorizonMs = 100;

/** 速度估计(逻辑像素/秒)。最近 100ms 内样本的最小二乘线性拟合。 */
export class VelocityTracker {
  readonly #t = new Float64Array(kHistorySize);
  readonly #x = new Float64Array(kHistorySize);
  readonly #y = new Float64Array(kHistorySize);
  #index = 0;
  #count = 0;

  /** 记录一个样本(时间毫秒, 位置)。 */
  addPosition(timeMs: number, position: Offset): void {
    this.#index = (this.#index + 1) % kHistorySize;
    this.#t[this.#index] = timeMs;
    this.#x[this.#index] = position.dx;
    this.#y[this.#index] = position.dy;
    if (this.#count < kHistorySize) this.#count++;
  }

  /** 估计速度;样本不足时为 0。 */
  getVelocity(): Offset {
    if (this.#count < 2) return Offset.zero;
    const newest = this.#t[this.#index]!;
    let n = 0;
    let st = 0, sx = 0, sy = 0, stt = 0, stx = 0, sty = 0;
    for (let k = 0; k < this.#count; k++) {
      const i = (this.#index - k + kHistorySize) % kHistorySize;
      const age = newest - this.#t[i]!;
      if (age > kHorizonMs) break;
      const t = -age / 1000;
      const x = this.#x[i]!;
      const y = this.#y[i]!;
      n++;
      st += t; sx += x; sy += y; stt += t * t; stx += t * x; sty += t * y;
    }
    if (n < 2) return Offset.zero;
    const denom = n * stt - st * st;
    if (Math.abs(denom) < 1e-12) return Offset.zero;
    const vx = (n * stx - st * sx) / denom;
    const vy = (n * sty - st * sy) / denom;
    const clamp = (v: number): number => Math.max(-kMaxFlingVelocity, Math.min(kMaxFlingVelocity, v));
    return new Offset(clamp(vx), clamp(vy));
  }

  reset(): void {
    this.#count = 0;
  }
}
