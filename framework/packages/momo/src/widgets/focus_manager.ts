import { ChangeNotifier } from "../foundation/change_notifier";
import { reportError } from "../foundation/assert";
import { HardwareKeyboard, type KeyEvent, type KeyEventResult } from "../services/hardware_keyboard";
import { RenderBox } from "../rendering/box";
import { Offset, Rect } from "../painting/geometry";
import type { BuildContext } from "./framework";

/** 焦点节点的按键回调。 */
export type FocusOnKeyEventCallback = (node: FocusNode, event: KeyEvent) => KeyEventResult;

/** 焦点节点与 widget 树的连接句柄。 */
export class FocusAttachment {
  constructor(readonly node: FocusNode) {}
  get isAttached(): boolean {
    return this.node._attachment === this;
  }
  /** 挂到 parent(缺省为根作用域)之下。 */
  reparent(parent?: FocusNode | null): void {
    if (!this.isAttached) return;
    const manager = FocusManager.instance;
    const p = parent ?? manager.rootScope;
    if (this.node.parent !== p) p._reparentChild(this.node);
  }
  /** 断开(若持有焦点则交还作用域)。 */
  detach(): void {
    if (!this.isAttached) return;
    const node = this.node;
    if (node.hasPrimaryFocus || (node.hasFocus && node instanceof FocusScopeNode)) {
      const scope = node.enclosingScope;
      FocusManager.instance._markDetached(node, scope);
    }
    node.parent?._removeChild(node);
    node._attachment = null;
    node._context = null;
  }
}

/**
 * 焦点树节点(对标 Flutter FocusNode)。按键事件从主焦点沿祖先链冒泡。
 */
export class FocusNode extends ChangeNotifier {
  readonly debugLabel: string | undefined;
  onKeyEvent: FocusOnKeyEventCallback | null;
  #skipTraversal: boolean;
  #canRequestFocus: boolean;
  #descendantsAreFocusable = true;
  /** @internal */ _parent: FocusNode | null = null;
  /** @internal */ readonly _children: FocusNode[] = [];
  /** @internal */ _attachment: FocusAttachment | null = null;
  /** @internal */ _context: BuildContext | null = null;
  #hadFocus = false;

  constructor(o: { debugLabel?: string; onKeyEvent?: FocusOnKeyEventCallback | null; skipTraversal?: boolean; canRequestFocus?: boolean } = {}) {
    super();
    this.debugLabel = o.debugLabel;
    this.onKeyEvent = o.onKeyEvent ?? null;
    this.#skipTraversal = o.skipTraversal ?? false;
    this.#canRequestFocus = o.canRequestFocus ?? true;
  }

  get parent(): FocusNode | null {
    return this._parent;
  }
  get children(): readonly FocusNode[] {
    return this._children;
  }
  get context(): BuildContext | null {
    return this._context;
  }
  get skipTraversal(): boolean {
    return this.#skipTraversal;
  }
  set skipTraversal(v: boolean) {
    this.#skipTraversal = v;
  }
  /** 能否获得焦点(祖先禁止后代聚焦时也为 false)。 */
  get canRequestFocus(): boolean {
    if (!this.#canRequestFocus) return false;
    for (let a = this._parent; a !== null; a = a._parent) if (!a.#descendantsAreFocusable) return false;
    return true;
  }
  set canRequestFocus(v: boolean) {
    if (v === this.#canRequestFocus) return;
    this.#canRequestFocus = v;
    if (!v && this.hasFocus) this.unfocus();
  }
  get descendantsAreFocusable(): boolean {
    return this.#descendantsAreFocusable;
  }
  set descendantsAreFocusable(v: boolean) {
    if (v === this.#descendantsAreFocusable) return;
    this.#descendantsAreFocusable = v;
    if (!v && this.hasFocus && !this.hasPrimaryFocus) this.unfocus();
  }

  /** 自身或后代是主焦点。 */
  get hasFocus(): boolean {
    const p = FocusManager.instance.primaryFocus;
    for (let n = p; n !== null; n = n._parent) if (n === this) return true;
    return false;
  }
  /** 自身是主焦点。 */
  get hasPrimaryFocus(): boolean {
    return FocusManager.instance.primaryFocus === this;
  }
  /** 最近的祖先作用域。 */
  get enclosingScope(): FocusScopeNode | null {
    for (let a = this._parent; a !== null; a = a._parent) if (a instanceof FocusScopeNode) return a;
    return null;
  }
  /** 祖先(由近及远)。 */
  get ancestors(): FocusNode[] {
    const out: FocusNode[] = [];
    for (let a = this._parent; a !== null; a = a._parent) out.push(a);
    return out;
  }
  /** 全部后代(深度优先)。 */
  get descendants(): FocusNode[] {
    const out: FocusNode[] = [];
    const walk = (n: FocusNode): void => {
      for (const c of n._children) {
        walk(c);
        out.push(c);
      }
    };
    walk(this);
    return out;
  }
  /** 可 Tab 遍历的后代。 */
  get traversalDescendants(): FocusNode[] {
    if (!this.#descendantsAreFocusable) return [];
    return this.descendants.filter((n) => !n.skipTraversal && n.canRequestFocus && !(n instanceof FocusScopeNode && n.traversalDescendants.length === 0 && !n.canRequestFocus));
  }

  /** 节点在窗口中的矩形(逻辑像素)。 */
  get rect(): Rect | null {
    const ro = this._context?.mounted ? this._context.findRenderObject() : null;
    if (!(ro instanceof RenderBox) || !ro.hasSize || !ro.attached) return null;
    const tl = ro.localToGlobal(Offset.zero);
    return Rect.fromLTWH(tl.dx, tl.dy, ro.size.width, ro.size.height);
  }

  /** 连接到 widget 树。 */
  attach(context: BuildContext | null, o: { onKeyEvent?: FocusOnKeyEventCallback | null } = {}): FocusAttachment {
    this._context = context;
    if (o.onKeyEvent !== undefined) this.onKeyEvent = o.onKeyEvent;
    this._attachment = new FocusAttachment(this);
    return this._attachment;
  }

  /** 请求成为主焦点。 */
  requestFocus(): void {
    if (!this.canRequestFocus) return;
    FocusManager.instance._setPrimary(this);
  }

  /** 放弃焦点:交还给最近作用域。 */
  unfocus(): void {
    if (!this.hasFocus) return;
    const scope = this instanceof FocusScopeNode ? this.enclosingScope ?? this : this.enclosingScope;
    if (scope === null) {
      FocusManager.instance._setPrimary(null);
      return;
    }
    if (scope.focusedChild === this || (scope.focusedChild !== null && this.descendants.includes(scope.focusedChild))) scope.focusedChild = null;
    FocusManager.instance._setPrimary(scope);
  }

  /** 移到下一个可遍历节点。 */
  nextFocus(): boolean {
    return FocusTraversalPolicy.instance.next(this, true);
  }
  /** 移到上一个可遍历节点。 */
  previousFocus(): boolean {
    return FocusTraversalPolicy.instance.next(this, false);
  }

  /** @internal */
  _reparentChild(child: FocusNode): void {
    if (child._parent === this) return;
    const hadFocus = child.hasFocus;
    child._parent?._removeChild(child, true);
    this._children.push(child);
    child._parent = this;
    if (hadFocus) FocusManager.instance._notifyChain();
  }
  /** @internal */
  _removeChild(child: FocusNode, _moving = false): void {
    const i = this._children.indexOf(child);
    if (i >= 0) this._children.splice(i, 1);
    child._parent = null;
    for (let s: FocusNode | null = this; s !== null; s = s._parent) {
      if (s instanceof FocusScopeNode && s.focusedChild !== null && (s.focusedChild === child || child.descendants.includes(s.focusedChild))) s.focusedChild = null;
    }
  }

  /** @internal 焦点状态变化时由 FocusManager 调用。 */
  _notifyIfChanged(): void {
    const has = this.hasFocus;
    if (has !== this.#hadFocus) {
      this.#hadFocus = has;
      this.notifyListeners();
    }
  }

  override dispose(): void {
    this._attachment?.detach();
    super.dispose();
  }

  toString(): string {
    return `FocusNode(${this.debugLabel ?? ""}${this.hasPrimaryFocus ? " PRIMARY" : this.hasFocus ? " focused" : ""})`;
  }
}

/** 焦点作用域:记住最近聚焦的后代, 作用域获得焦点时恢复给它。 */
export class FocusScopeNode extends FocusNode {
  /** 最近获得焦点的后代。 */
  focusedChild: FocusNode | null = null;

  constructor(o: { debugLabel?: string; onKeyEvent?: FocusOnKeyEventCallback | null; skipTraversal?: boolean; canRequestFocus?: boolean } = {}) {
    super(o);
  }

  override requestFocus(): void {
    if (!this.canRequestFocus) return;
    const f = this.focusedChild;
    if (f !== null && f.canRequestFocus && f.parent !== null) f.requestFocus();
    else FocusManager.instance._setPrimary(this);
  }

  /** 若尚无聚焦后代, 聚焦 node。 */
  autofocus(node: FocusNode): void {
    if (this.focusedChild === null) node.requestFocus();
  }
}

/** 焦点管理器(单例):主焦点、按键分发。 */
export class FocusManager extends ChangeNotifier {
  static #instance: FocusManager | null = null;
  static get instance(): FocusManager {
    if (FocusManager.#instance === null) FocusManager.#instance = new FocusManager();
    return FocusManager.#instance;
  }

  readonly rootScope = new FocusScopeNode({ debugLabel: "Root Focus Scope" });
  #primary: FocusNode | null = null;
  #keyboardHandler = (event: KeyEvent): boolean => this.handleKeyEvent(event);
  #registered = false;

  private constructor() {
    super();
    this.rootScope._attachment = new FocusAttachment(this.rootScope);
  }

  /** 接入 HardwareKeyboard(幂等)。 */
  registerGlobalHandlers(): void {
    if (this.#registered) return;
    this.#registered = true;
    HardwareKeyboard.instance.addHandler(this.#keyboardHandler);
  }

  /** 当前主焦点。 */
  get primaryFocus(): FocusNode | null {
    return this.#primary;
  }

  /** @internal */
  _setPrimary(node: FocusNode | null): void {
    if (node === this.#primary) return;
    const old = this.#primary;
    this.#primary = node;
    if (node !== null) {
      // 记录各级作用域的 focusedChild
      let child: FocusNode = node;
      for (let s = node.enclosingScope; s !== null; s = s.enclosingScope) {
        s.focusedChild = child;
        child = s;
      }
    }
    this.#notifyNodes(old, node);
    this.notifyListeners();
  }

  /** @internal 节点被移除时转移焦点。 */
  _markDetached(node: FocusNode, scope: FocusScopeNode | null): void {
    if (this.#primary === null) return;
    let inside = false;
    for (let n: FocusNode | null = this.#primary; n !== null; n = n._parent) if (n === node) inside = true;
    if (!inside) return;
    if (scope !== null && scope.focusedChild !== null && (scope.focusedChild === node || node.descendants.includes(scope.focusedChild))) scope.focusedChild = null;
    const old = this.#primary;
    this.#primary = scope !== null && scope !== node ? scope : null;
    this.#notifyNodes(old, this.#primary, node);
    this.notifyListeners();
  }

  /** @internal */
  _notifyChain(): void {
    this.#notifyNodes(this.#primary, this.#primary);
  }

  #notifyNodes(a: FocusNode | null, b: FocusNode | null, extra: FocusNode | null = null): void {
    const set = new Set<FocusNode>();
    for (const start of [a, b, extra]) for (let n = start; n !== null; n = n._parent) set.add(n);
    for (const n of set) {
      try {
        n._notifyIfChanged();
      } catch (e) {
        reportError(e, "while notifying focus listeners");
      }
    }
  }

  /** 按键分发:从主焦点(无则根作用域)沿祖先链调用 onKeyEvent。 */
  handleKeyEvent(event: KeyEvent): boolean {
    let node: FocusNode | null = this.#primary ?? this.rootScope;
    while (node !== null) {
      const cb = node.onKeyEvent;
      if (cb !== null) {
        let r: KeyEventResult = "ignored";
        try {
          r = cb(node, event);
        } catch (e) {
          reportError(e, "in a FocusNode.onKeyEvent handler");
        }
        if (r === "handled") return true;
        if (r === "skipRemainingHandlers") return false;
      }
      node = node._parent;
    }
    return false;
  }

  /** 测试用:清空焦点树。 */
  debugReset(): void {
    this.#primary = null;
    this.rootScope.focusedChild = null;
    for (const c of [...this.rootScope.children]) this.rootScope._removeChild(c);
  }
}

/** 焦点遍历策略:阅读顺序(先上后下、同行从左到右), 在最近作用域内循环。 */
export class FocusTraversalPolicy {
  static instance = new FocusTraversalPolicy();

  /** 作用域内按阅读顺序排序的候选。 */
  sortedCandidates(scope: FocusScopeNode): FocusNode[] {
    const nodes = scope.traversalDescendants.filter((n) => !(n instanceof FocusScopeNode));
    const withRect = nodes.map((n, i) => ({ n, r: n.rect, i }));
    withRect.sort((x, y) => {
      if (x.r === null || y.r === null) return x.i - y.i;
      const dy = x.r.top - y.r.top;
      if (Math.abs(dy) > Math.min(x.r.height, y.r.height) / 2) return dy;
      return x.r.left - y.r.left || x.i - y.i;
    });
    return withRect.map((x) => x.n);
  }

  /** 从 current 移动到下一个/上一个;返回是否移动。 */
  next(current: FocusNode, forward: boolean): boolean {
    const scope = current instanceof FocusScopeNode && current.hasPrimaryFocus ? current : current.enclosingScope;
    if (scope === null) return false;
    const list = this.sortedCandidates(scope);
    if (list.length === 0) return false;
    const i = list.indexOf(current);
    let target: FocusNode;
    if (i < 0) target = forward ? list[0]! : list[list.length - 1]!;
    else target = list[(i + (forward ? 1 : list.length - 1)) % list.length]!;
    if (target === current) return false;
    target.requestFocus();
    return true;
  }
}
