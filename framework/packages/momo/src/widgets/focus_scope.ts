import type { Key } from "../foundation/key";
import type { KeyEvent, KeyEventResult } from "../services/hardware_keyboard";
import { SchedulerBinding } from "../scheduler/binding";
import {
  InheritedWidget, State, StatefulWidget, type BuildContext, type Widget,
} from "./framework";
import { FocusAttachment, FocusManager, FocusNode, FocusScopeNode, type FocusOnKeyEventCallback } from "./focus_manager";

/** 向子树提供最近的 FocusNode。 */
class FocusMarker extends InheritedWidget {
  constructor(readonly node: FocusNode, child: Widget) {
    super({ child });
  }
  updateShouldNotify(old: FocusMarker): boolean {
    return old.node !== this.node;
  }
}

/** Focus 构造参数。 */
export interface FocusOptions {
  key?: Key | null;
  child: Widget;
  focusNode?: FocusNode | null;
  autofocus?: boolean;
  onFocusChange?: ((focused: boolean) => void) | null;
  onKeyEvent?: FocusOnKeyEventCallback | null;
  canRequestFocus?: boolean;
  skipTraversal?: boolean;
  descendantsAreFocusable?: boolean;
  debugLabel?: string;
  /**
   * 焦点树中的父节点(缺省为 widget 树中最近的 Focus)。多窗口: 每个窗口的根作用域挂在
   * FocusManager.rootScope 下, 与主窗口的作用域并列, 而不是它的后代。
   */
  parentNode?: FocusNode | null;
}

/** 管理一个 FocusNode 并把它挂入焦点树(位置由 widget 树决定)。 */
export class Focus extends StatefulWidget {
  readonly child: Widget;
  readonly focusNode: FocusNode | null;
  readonly autofocus: boolean;
  readonly onFocusChange: ((focused: boolean) => void) | null;
  readonly onKeyEvent: FocusOnKeyEventCallback | null;
  readonly canRequestFocus: boolean | null;
  readonly skipTraversal: boolean | null;
  readonly descendantsAreFocusable: boolean | null;
  readonly debugLabel: string | undefined;
  readonly parentNode: FocusNode | null;

  constructor(o: FocusOptions) {
    super(o.key);
    this.child = o.child;
    this.focusNode = o.focusNode ?? null;
    this.autofocus = o.autofocus ?? false;
    this.onFocusChange = o.onFocusChange ?? null;
    this.onKeyEvent = o.onKeyEvent ?? null;
    this.canRequestFocus = o.canRequestFocus ?? null;
    this.skipTraversal = o.skipTraversal ?? null;
    this.descendantsAreFocusable = o.descendantsAreFocusable ?? null;
    this.debugLabel = o.debugLabel;
    this.parentNode = o.parentNode ?? null;
  }

  createState(): State {
    return new FocusState();
  }

  /** 最近的 FocusNode(注册依赖)。 */
  static maybeOf(context: BuildContext): FocusNode | null {
    return context.dependOnInheritedWidgetOfExactType(FocusMarker)?.node ?? null;
  }
  static of(context: BuildContext): FocusNode {
    const n = Focus.maybeOf(context);
    if (n === null) throw new Error("No Focus ancestor");
    return n;
  }
  /** 子树中是否有焦点。 */
  static isAt(context: BuildContext): boolean {
    return Focus.maybeOf(context)?.hasFocus ?? false;
  }

  /** @internal */
  createNode(): FocusNode {
    return new FocusNode({ debugLabel: this.debugLabel ?? "Focus" });
  }
}

export class FocusState extends State<Focus> {
  #internal: FocusNode | null = null;
  #attachment: FocusAttachment | null = null;
  #hadFocus = false;
  #didAutofocus = false;

  get focusNode(): FocusNode {
    return this.widget.focusNode ?? this.#internal!;
  }

  override initState(): void {
    if (this.widget.focusNode === null) this.#internal = this.widget.createNode();
    this.#attach();
  }

  #applyProps(): void {
    const w = this.widget;
    const n = this.focusNode;
    if (w.canRequestFocus !== null) n.canRequestFocus = w.canRequestFocus;
    if (w.skipTraversal !== null) n.skipTraversal = w.skipTraversal;
    if (w.descendantsAreFocusable !== null) n.descendantsAreFocusable = w.descendantsAreFocusable;
  }

  #attach(): void {
    this.#applyProps();
    const node = this.focusNode;
    this.#attachment = node.attach(this.context, { onKeyEvent: this.#onKey });
    this.#hadFocus = node.hasFocus;
    node.addListener(this.#handleFocusChanged);
  }

  #onKey = (node: FocusNode, event: KeyEvent): KeyEventResult => {
    const cb = this.widget.onKeyEvent;
    return cb !== null ? cb(node, event) : "ignored";
  };

  #handleFocusChanged = (): void => {
    const has = this.focusNode.hasFocus;
    if (has !== this.#hadFocus) {
      this.#hadFocus = has;
      this.widget.onFocusChange?.(has);
      if (this.mounted) this.setState();
    }
  };

  override didChangeDependencies(): void {
    const parent = this.widget.parentNode ?? Focus.maybeOf(this.context);
    this.#attachment?.reparent(parent);
    if (this.widget.autofocus && !this.#didAutofocus) {
      this.#didAutofocus = true;
      SchedulerBinding.instance.addPostFrameCallback(() => {
        if (!this.mounted) return;
        const node = this.focusNode;
        const scope = node instanceof FocusScopeNode ? node.enclosingScope : node.enclosingScope;
        if (scope !== null) scope.autofocus(node);
        else node.requestFocus();
      });
      SchedulerBinding.instance.ensureVisualUpdate();
    }
  }

  override didUpdateWidget(old: Focus): void {
    if (old.focusNode !== this.widget.focusNode) {
      const oldNode = old.focusNode ?? this.#internal!;
      oldNode.removeListener(this.#handleFocusChanged);
      this.#attachment?.detach();
      if (this.widget.focusNode === null && this.#internal === null) this.#internal = this.widget.createNode();
      this.#attach();
      this.#attachment?.reparent(this.widget.parentNode ?? Focus.maybeOf(this.context));
    } else {
      this.#applyProps();
    }
  }

  override dispose(): void {
    this.focusNode.removeListener(this.#handleFocusChanged);
    this.#attachment?.detach();
    this.#internal?.dispose();
  }

  build(_context: BuildContext): Widget {
    return new FocusMarker(this.focusNode, this.widget.child);
  }
}

/** 焦点作用域 widget(FocusScopeNode)。 */
export class FocusScope extends Focus {
  constructor(o: Omit<FocusOptions, "focusNode"> & { node?: FocusScopeNode | null }) {
    super({ ...o, focusNode: o.node ?? null });
  }
  override createNode(): FocusNode {
    return new FocusScopeNode({ debugLabel: this.debugLabel ?? "FocusScope" });
  }
  /** 最近的作用域。 */
  static of(context: BuildContext): FocusScopeNode {
    const n = Focus.maybeOf(context);
    if (n instanceof FocusScopeNode) return n;
    return n?.enclosingScope ?? FocusManager.instance.rootScope;
  }
}

/** 当前主焦点。 */
export function primaryFocus(): FocusNode | null {
  return FocusManager.instance.primaryFocus;
}
