import { reportError } from "../foundation/assert";

/** 竞技场裁决。 */
export type GestureDisposition = "accepted" | "rejected";

/** 竞技场成员(手势识别器)。 */
export interface GestureArenaMember {
  /** 赢得该指针的竞技场。 */
  acceptGesture(pointer: number): void;
  /** 输掉该指针的竞技场。 */
  rejectGesture(pointer: number): void;
}

/** 成员在某指针竞技场中的席位。 */
export class GestureArenaEntry {
  constructor(readonly arena: GestureArenaManager, readonly pointer: number, readonly member: GestureArenaMember) {}
  /** 宣告接受或放弃。 */
  resolve(disposition: GestureDisposition): void {
    this.arena._resolve(this.pointer, this.member, disposition);
  }
}

class Arena {
  readonly members: GestureArenaMember[] = [];
  isOpen = true;
  isHeld = false;
  hasPendingSweep = false;
  eagerWinner: GestureArenaMember | null = null;
}

/**
 * 手势竞技场(对标 Flutter GestureArenaManager):每个指针一场;按下时 close, 抬起时 sweep;
 * 只剩一名成员时(微任务中)默认获胜;成员可 hold 延迟 sweep(双击)。
 */
export class GestureArenaManager {
  #arenas = new Map<number, Arena>();

  /** 加入指针的竞技场。 */
  add(pointer: number, member: GestureArenaMember): GestureArenaEntry {
    let state = this.#arenas.get(pointer);
    if (state === undefined) {
      state = new Arena();
      this.#arenas.set(pointer, state);
    }
    state.members.push(member);
    return new GestureArenaEntry(this, pointer, member);
  }

  /** 停止接受新成员(PointerDown 分发完成后)。 */
  close(pointer: number): void {
    const state = this.#arenas.get(pointer);
    if (state === undefined) return;
    state.isOpen = false;
    this.#tryToResolveArena(pointer, state);
  }

  /** 强制裁决:第一个成员获胜(PointerUp 时)。 */
  sweep(pointer: number): void {
    const state = this.#arenas.get(pointer);
    if (state === undefined) return;
    if (state.isHeld) {
      state.hasPendingSweep = true;
      return;
    }
    this.#arenas.delete(pointer);
    if (state.members.length > 0) {
      const [first, ...rest] = state.members;
      this.#call(() => first!.acceptGesture(pointer));
      for (const m of rest) this.#call(() => m.rejectGesture(pointer));
    }
  }

  /** 推迟 sweep。 */
  hold(pointer: number): void {
    const state = this.#arenas.get(pointer);
    if (state !== undefined) state.isHeld = true;
  }

  /** 取消 hold;若有待处理的 sweep 则立即执行。 */
  release(pointer: number): void {
    const state = this.#arenas.get(pointer);
    if (state === undefined) return;
    state.isHeld = false;
    if (state.hasPendingSweep) this.sweep(pointer);
  }

  /** 指针竞技场当前的成员数(按下分发过程中: 已加入的都是命中路径上更深的目标)。 */
  memberCount(pointer: number): number {
    return this.#arenas.get(pointer)?.members.length ?? 0;
  }

  /** 是否存在该指针的竞技场。 */
  hasArena(pointer: number): boolean {
    return this.#arenas.has(pointer);
  }

  /** @internal */
  _resolve(pointer: number, member: GestureArenaMember, disposition: GestureDisposition): void {
    const state = this.#arenas.get(pointer);
    if (state === undefined) return;
    if (disposition === "rejected") {
      const i = state.members.indexOf(member);
      if (i < 0) return;
      state.members.splice(i, 1);
      this.#call(() => member.rejectGesture(pointer));
      if (!state.isOpen) this.#tryToResolveArena(pointer, state);
    } else {
      if (state.isOpen) {
        state.eagerWinner ??= member;
      } else {
        this.#resolveInFavorOf(pointer, state, member);
      }
    }
  }

  #tryToResolveArena(pointer: number, state: Arena): void {
    if (state.members.length === 1) {
      void Promise.resolve().then(() => this.#resolveByDefault(pointer, state));
    } else if (state.members.length === 0) {
      this.#arenas.delete(pointer);
    } else if (state.eagerWinner !== null) {
      this.#resolveInFavorOf(pointer, state, state.eagerWinner);
    }
  }

  #resolveByDefault(pointer: number, state: Arena): void {
    if (this.#arenas.get(pointer) !== state) return;
    if (state.members.length !== 1) return;
    this.#arenas.delete(pointer);
    const m = state.members[0]!;
    this.#call(() => m.acceptGesture(pointer));
  }

  #resolveInFavorOf(pointer: number, state: Arena, member: GestureArenaMember): void {
    this.#arenas.delete(pointer);
    for (const m of state.members) if (m !== member) this.#call(() => m.rejectGesture(pointer));
    this.#call(() => member.acceptGesture(pointer));
  }

  #call(fn: () => void): void {
    try {
      fn();
    } catch (e) {
      reportError(e, "in a gesture arena callback");
    }
  }

  /** 测试用:清空。 */
  reset(): void {
    this.#arenas.clear();
  }
}
