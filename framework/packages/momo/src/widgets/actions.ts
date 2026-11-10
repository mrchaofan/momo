import type { Key } from "../foundation/key";
import { HardwareKeyboard, KeyDownEvent, KeyRepeatEvent, type KeyEvent, type KeyEventResult } from "../services/hardware_keyboard";
import { LogicalKeyboardKey } from "../services/keyboard_key";
import { InheritedWidget, StatelessWidget, type BuildContext, type Type, type Widget } from "./framework";
import { Focus } from "./focus_scope";
import { FocusManager, type FocusNode } from "./focus_manager";

/** 意图:描述"要做什么", 由 Actions 映射到具体行为。 */
export abstract class Intent {}

/** 激活(Enter/Space 于按钮)。 */
export class ActivateIntent extends Intent {}
/** 下一个焦点(Tab)。 */
export class NextFocusIntent extends Intent {}
/** 上一个焦点(Shift+Tab)。 */
export class PreviousFocusIntent extends Intent {}
/** 关闭(Escape)。 */
export class DismissIntent extends Intent {}
/** 占位:吞掉按键。 */
export class DoNothingIntent extends Intent {}

/** 行为。 */
export abstract class Action<T extends Intent = Intent> {
  /** 是否可用;不可用时继续向上查找。 */
  isEnabled(_intent: T): boolean {
    return true;
  }
  /** 执行;返回值透传给调用者。 */
  abstract invoke(intent: T): unknown;
  /** 触发该行为的按键是否视为已消费。 */
  consumesKey(_intent: T): boolean {
    return true;
  }
}

/** 回调实现的行为。 */
export class CallbackAction<T extends Intent = Intent> extends Action<T> {
  readonly #onInvoke: (intent: T) => unknown;
  readonly #enabled: ((intent: T) => boolean) | null;
  constructor(o: { onInvoke: (intent: T) => unknown; isEnabled?: (intent: T) => boolean }) {
    super();
    this.#onInvoke = o.onInvoke;
    this.#enabled = o.isEnabled ?? null;
  }
  override isEnabled(intent: T): boolean {
    return this.#enabled === null || this.#enabled(intent);
  }
  invoke(intent: T): unknown {
    return this.#onInvoke(intent);
  }
}

/** 不做任何事但消费按键。 */
export class DoNothingAction extends Action<Intent> {
  readonly #consumes: boolean;
  constructor(consumesKey = true) {
    super();
    this.#consumes = consumesKey;
  }
  invoke(): unknown {
    return null;
  }
  override consumesKey(): boolean {
    return this.#consumes;
  }
}

/** 意图类型 → 行为 的映射。 */
export type ActionMap = ReadonlyMap<Type<Intent>, Action<never>>;

class ActionsMarker extends InheritedWidget {
  constructor(readonly actions: ActionMap, child: Widget) {
    super({ child });
  }
  updateShouldNotify(old: ActionsMarker): boolean {
    return old.actions !== this.actions;
  }
}

/** 为子树提供意图 → 行为映射。 */
export class Actions extends StatelessWidget {
  readonly actions: ActionMap;
  readonly child: Widget;
  constructor(o: { key?: Key | null; actions: ActionMap; child: Widget }) {
    super(o.key);
    this.actions = o.actions;
    this.child = o.child;
  }
  build(_context: BuildContext): Widget {
    return new ActionsMarker(this.actions, this.child);
  }

  /** 从 context 向上查找能处理 intent 的已启用行为。 */
  static maybeFind<T extends Intent>(context: BuildContext, intent: T): Action<T> | null {
    let e: BuildContext | null = context;
    while (e !== null) {
      if (e.widget instanceof ActionsMarker) {
        const a = e.widget.actions.get(intent.constructor as Type<Intent>) as Action<T> | undefined;
        if (a !== undefined && a.isEnabled(intent)) return a;
      }
      e = e.parent;
    }
    return null;
  }

  /** 调用行为;找不到返回 undefined。 */
  static maybeInvoke<T extends Intent>(context: BuildContext, intent: T): { result: unknown; consumed: boolean } | undefined {
    const a = Actions.maybeFind(context, intent);
    if (a === null) return undefined;
    return { result: a.invoke(intent), consumed: a.consumesKey(intent) };
  }

  static invoke<T extends Intent>(context: BuildContext, intent: T): unknown {
    return Actions.maybeInvoke(context, intent)?.result;
  }
}

/** 快捷键触发条件。 */
export interface ShortcutActivator {
  accepts(event: KeyEvent, keyboard: HardwareKeyboard): boolean;
}

/** 单键 + 修饰键组合(修饰键必须精确匹配)。 */
export class SingleActivator implements ShortcutActivator {
  readonly trigger: LogicalKeyboardKey;
  readonly control: boolean;
  readonly shift: boolean;
  readonly alt: boolean;
  readonly meta: boolean;
  readonly includeRepeats: boolean;
  constructor(trigger: LogicalKeyboardKey, o: { control?: boolean; shift?: boolean; alt?: boolean; meta?: boolean; includeRepeats?: boolean } = {}) {
    this.trigger = trigger;
    this.control = o.control ?? false;
    this.shift = o.shift ?? false;
    this.alt = o.alt ?? false;
    this.meta = o.meta ?? false;
    this.includeRepeats = o.includeRepeats ?? true;
  }
  accepts(event: KeyEvent, keyboard: HardwareKeyboard): boolean {
    if (!(event instanceof KeyDownEvent) && !(this.includeRepeats && event instanceof KeyRepeatEvent)) return false;
    if (event.logicalKey !== this.trigger) return false;
    return keyboard.isControlPressed === this.control && keyboard.isShiftPressed === this.shift &&
      keyboard.isAltPressed === this.alt && keyboard.isMetaPressed === this.meta;
  }
}

/** 快捷键表:按键 → 意图;经焦点链冒泡收到按键时, 从主焦点处调用 Actions。 */
export class Shortcuts extends StatelessWidget {
  readonly shortcuts: ReadonlyMap<ShortcutActivator, Intent>;
  readonly child: Widget;
  readonly debugLabel: string | undefined;
  constructor(o: { key?: Key | null; shortcuts: ReadonlyMap<ShortcutActivator, Intent>; child: Widget; debugLabel?: string }) {
    super(o.key);
    this.shortcuts = o.shortcuts;
    this.child = o.child;
    this.debugLabel = o.debugLabel;
  }

  /** 处理按键(也供测试直接调用)。 */
  handleKeypress(context: BuildContext, event: KeyEvent): KeyEventResult {
    const keyboard = HardwareKeyboard.instance;
    for (const [activator, intent] of this.shortcuts) {
      if (!activator.accepts(event, keyboard)) continue;
      const primary = FocusManager.instance.primaryFocus;
      const ctx = primary?.context?.mounted ? primary.context : context;
      const r = Actions.maybeInvoke(ctx, intent);
      if (r !== undefined) return r.consumed ? "handled" : "skipRemainingHandlers";
    }
    return "ignored";
  }

  build(context: BuildContext): Widget {
    return new Focus({
      debugLabel: this.debugLabel ?? "Shortcuts",
      canRequestFocus: false,
      skipTraversal: true,
      onKeyEvent: (_node: FocusNode, event: KeyEvent) => this.handleKeypress(context, event),
      child: this.child,
    });
  }
}

/** 默认快捷键(Tab/Shift+Tab 遍历, Enter/Space 激活, Escape 关闭)。 */
export function defaultShortcuts(): Map<ShortcutActivator, Intent> {
  return new Map<ShortcutActivator, Intent>([
    [new SingleActivator(LogicalKeyboardKey.tab), new NextFocusIntent()],
    [new SingleActivator(LogicalKeyboardKey.tab, { shift: true }), new PreviousFocusIntent()],
    [new SingleActivator(LogicalKeyboardKey.enter), new ActivateIntent()],
    [new SingleActivator(LogicalKeyboardKey.space), new ActivateIntent()],
    [new SingleActivator(LogicalKeyboardKey.escape), new DismissIntent()],
  ]);
}

/** 默认行为(焦点遍历)。 */
export function defaultActions(): Map<Type<Intent>, Action<never>> {
  return new Map<Type<Intent>, Action<never>>([
    [NextFocusIntent, new CallbackAction<NextFocusIntent>({ onInvoke: () => FocusManager.instance.primaryFocus?.nextFocus() ?? false })],
    [PreviousFocusIntent, new CallbackAction<PreviousFocusIntent>({ onInvoke: () => FocusManager.instance.primaryFocus?.previousFocus() ?? false })],
  ]);
}
