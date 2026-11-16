// 测试用假时钟: 代替宿主定时器驱动 SchedulerBinding.scheduleDelayed, 时间只由测试推进(ViewTester.pump),
// 因此延迟任务(光标闪烁、双击超时、长按…)在 mock 宿主和真实引擎下都是确定的。导入本模块即安装。
import { SchedulerBinding, type TimerHost } from "../packages/momo/src/index";

interface FakeTimer {
  readonly id: number;
  readonly deadline: number;
  readonly callback: () => void;
}

export class FakeClock implements TimerHost {
  /** 当前时间(毫秒)。 */
  now = 1000;
  #nextId = 0;
  readonly #timers = new Map<number, FakeTimer>();

  setTimer(callback: () => void, delayMs: number): unknown {
    const id = ++this.#nextId;
    this.#timers.set(id, { id, deadline: this.now + delayMs, callback });
    return id;
  }
  clearTimer(handle: unknown): void {
    this.#timers.delete(handle as number);
  }

  /** 尚未触发的定时器数。 */
  get pendingTimers(): number {
    return this.#timers.size;
  }

  /**
   * 时间前进 ms, 按到期顺序(同时到期的按登记顺序)执行到期的定时器;回调执行时 now 是它的到期时间, 回调里
   * 新登记的定时器若也在这段时间内到期, 同样执行。
   */
  advance(ms: number): void {
    const target = this.now + ms;
    for (;;) {
      let next: FakeTimer | null = null;
      for (const t of this.#timers.values()) {
        if (t.deadline <= target && (next === null || t.deadline < next.deadline)) next = t;
      }
      if (next === null) break;
      this.#timers.delete(next.id);
      this.now = Math.max(this.now, next.deadline);
      next.callback();
    }
    this.now = target;
  }
}

/** 生产环境的定时器来源(宿主定时器): 假时钟安装前的缺省值, 留给验证真实路径的测试。 */
export const hostTimerHost: TimerHost = SchedulerBinding.instance.timerHost;

export const fakeClock = new FakeClock();
SchedulerBinding.instance.timerHost = fakeClock;
