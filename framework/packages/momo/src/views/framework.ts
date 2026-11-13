import { Paint } from "momo:ui";
import { MomoError, reportError } from "../foundation/assert";
import { describeIdentity } from "../foundation/diagnostics";
import { Key, keyHashOf, keysEqual } from "../foundation/key";
import type { Offset, Size } from "../painting/geometry";
import { RenderBox } from "../rendering/box";
import { ContainerRenderBox } from "../rendering/box";
import type { PaintingContext, RenderObject } from "../rendering/object";
import { Ticker, type TickerCallback, type TickerProvider } from "../scheduler/ticker";

/** 构造函数类型(用于按类型查找祖先/依赖)。 */
export type Type<T> = abstract new (...args: never[]) => T;

/**
 * 不可变的界面配置。canUpdate 判定 = 构造函数相同 且 key 相等。
 */
export abstract class View {
  readonly key: Key | null;
  constructor(key?: Key | null) {
    this.key = key ?? null;
  }
  /** 创建对应的 Element。 */
  abstract createElement(): Element;
  /** 旧 Element 能否用新 view 原地更新。 */
  static canUpdate(oldView: View, newView: View): boolean {
    return oldView.constructor === newView.constructor && keysEqual(oldView.key, newView.key);
  }
  toStringShort(): string {
    return this.key === null ? this.constructor.name : `${this.constructor.name}-${String(this.key)}`;
  }
}

/** 构建上下文 = 当前 Element。 */
export type BuildContext = Element;

/** 多子节点的槽位:下标 + 前一个兄弟 Element。 */
export class IndexedSlot {
  constructor(readonly index: number, readonly value: Element | null) {}
  equals(o: unknown): boolean {
    return o instanceof IndexedSlot && o.index === this.index && o.value === this.value;
  }
}

function slotsEqual(a: unknown, b: unknown): boolean {
  if (a === b) return true;
  return a instanceof IndexedSlot && a.equals(b);
}

type Lifecycle = "initial" | "active" | "inactive" | "defunct";

const globalKeyRegistry = new Map<GlobalKey, Element>();

/** 全局唯一 key:可跨父节点移动并保留状态, 可取得 currentState/currentContext。 */
export class GlobalKey<T extends State = State> extends Key {
  readonly debugLabel: string | undefined;
  constructor(debugLabel?: string) {
    super();
    this.debugLabel = debugLabel;
  }
  equals(other: Key): boolean {
    return other === this;
  }
  /** 当前持有该 key 的 Element。 */
  get currentElement(): Element | null {
    return globalKeyRegistry.get(this) ?? null;
  }
  get currentContext(): BuildContext | null {
    return this.currentElement;
  }
  get currentView(): View | null {
    return this.currentElement?.view ?? null;
  }
  get currentState(): T | null {
    const e = this.currentElement;
    return e instanceof StatefulElement ? (e.state as T) : null;
  }
  toString(): string {
    return `[GlobalKey${this.debugLabel ? " " + this.debugLabel : ""}]`;
  }
}

/** 以 State 类型标注的 GlobalKey。 */
export class LabeledGlobalKey<T extends State = State> extends GlobalKey<T> {}

/**
 * View 树的持久化节点:持有位置(slot)、负责子节点 diff 与复用。
 */
export abstract class Element {
  #view: View | null;
  parent: Element | null = null;
  slot: unknown = null;
  depth = 0;
  owner: BuildOwner | null = null;
  #lifecycle: Lifecycle = "initial";
  /** 需要重建。 */
  dirty = true;
  /** @internal */ inDirtyList = false;
  /** @internal */ _inheritedElements: Map<Function, InheritedElement> | null = null;
  /** @internal */ _dependencies: Set<InheritedElement> | null = null;
  #hadUnsatisfiedDependencies = false;

  constructor(view: View) {
    this.#view = view;
  }

  /** 当前配置。 */
  get view(): View {
    if (this.#view === null) throw new MomoError("Element was used after being unmounted");
    return this.#view;
  }
  /** 是否已挂载且未卸载。 */
  get mounted(): boolean {
    return this.#view !== null && this.#lifecycle !== "defunct" && this.#lifecycle !== "initial";
  }
  /** @internal */
  get lifecycle(): Lifecycle {
    return this.#lifecycle;
  }
  get debugIsActive(): boolean {
    return this.#lifecycle === "active";
  }

  /** 遍历子 Element。 */
  visitChildren(_visitor: (child: Element) => void): void {}

  /** 遍历子 Element(构建上下文版本)。 */
  visitChildElements(visitor: (child: Element) => void): void {
    this.visitChildren(visitor);
  }

  /** 子 Element 列表(调试/测试)。 */
  get debugChildren(): Element[] {
    const out: Element[] = [];
    this.visitChildren((c) => out.push(c));
    return out;
  }

  /** 挂载到 parent 的 slot。 */
  mount(parent: Element | null, newSlot: unknown): void {
    this.parent = parent;
    this.slot = newSlot;
    this.#lifecycle = "active";
    this.depth = parent !== null ? parent.depth + 1 : 1;
    if (parent !== null) this.owner = parent.owner;
    const key = this.view.key;
    if (key instanceof GlobalKey) globalKeyRegistry.set(key, this);
    this._updateInheritance();
  }

  /** 用新配置更新(构造函数与 key 相同)。 */
  update(newView: View): void {
    this.#view = newView;
  }

  /** @internal */
  _updateInheritance(): void {
    this._inheritedElements = this.parent?._inheritedElements ?? null;
  }

  /**
   * 更新子节点(Flutter Element.updateChild 语义):
   * newView 为 null → 移除;可原地更新 → update;否则停用旧的并 inflate 新的。
   */
  updateChild(child: Element | null, newView: View | null, newSlot: unknown): Element | null {
    if (newView === null) {
      if (child !== null) this.deactivateChild(child);
      return null;
    }
    if (child !== null) {
      if (child.#view === newView) {
        if (!slotsEqual(child.slot, newSlot)) this.updateSlotForChild(child, newSlot);
        return child;
      }
      if (View.canUpdate(child.view, newView)) {
        if (!slotsEqual(child.slot, newSlot)) this.updateSlotForChild(child, newSlot);
        child.update(newView);
        return child;
      }
      this.deactivateChild(child);
    }
    return this.inflateView(newView, newSlot);
  }

  /**
   * 带 key 的列表 diff(Flutter updateChildren 算法):头尾同步, 中段按 key 匹配, 其余新建/停用。
   */
  updateChildren(oldChildren: readonly Element[], newViews: readonly View[], forgottenChildren: Set<Element> | null = null): Element[] {
    const replace = (e: Element | undefined): Element | null =>
      e === undefined || (forgottenChildren !== null && forgottenChildren.has(e)) ? null : e;
    let newTop = 0;
    let oldTop = 0;
    let newBottom = newViews.length - 1;
    let oldBottom = oldChildren.length - 1;
    const newChildren: Element[] = new Array<Element>(newViews.length);
    let previous: Element | null = null;

    while (oldTop <= oldBottom && newTop <= newBottom) {
      const oldChild = replace(oldChildren[oldTop]);
      const w = newViews[newTop]!;
      if (oldChild === null || !View.canUpdate(oldChild.view, w)) break;
      const nc: Element = this.updateChild(oldChild, w, new IndexedSlot(newTop, previous))!;
      newChildren[newTop] = nc;
      previous = nc;
      newTop++;
      oldTop++;
    }
    while (oldTop <= oldBottom && newTop <= newBottom) {
      const oldChild = replace(oldChildren[oldBottom]);
      const w = newViews[newBottom]!;
      if (oldChild === null || !View.canUpdate(oldChild.view, w)) break;
      oldBottom--;
      newBottom--;
    }
    const haveOld = oldTop <= oldBottom;
    const keyed = new Map<unknown, Element[]>();
    if (haveOld) {
      while (oldTop <= oldBottom) {
        const oldChild = replace(oldChildren[oldTop]);
        if (oldChild !== null) {
          const k = oldChild.view.key;
          if (k !== null) {
            const h = keyHashOf(k);
            const bucket = keyed.get(h);
            if (bucket) bucket.push(oldChild);
            else keyed.set(h, [oldChild]);
          } else {
            this.deactivateChild(oldChild);
          }
        }
        oldTop++;
      }
    }
    while (newTop <= newBottom) {
      let oldChild: Element | null = null;
      const w = newViews[newTop]!;
      if (haveOld && w.key !== null) {
        const bucket = keyed.get(keyHashOf(w.key));
        if (bucket) {
          const i = bucket.findIndex((e) => View.canUpdate(e.view, w));
          if (i >= 0) {
            oldChild = bucket[i]!;
            bucket.splice(i, 1);
          }
        }
      }
      const nc: Element = this.updateChild(oldChild, w, new IndexedSlot(newTop, previous))!;
      newChildren[newTop] = nc;
      previous = nc;
      newTop++;
    }
    newBottom = newViews.length - 1;
    oldBottom = oldChildren.length - 1;
    while (oldTop <= oldBottom && newTop <= newBottom) {
      const oldChild = oldChildren[oldTop]!;
      const w = newViews[newTop]!;
      const nc: Element = this.updateChild(oldChild, w, new IndexedSlot(newTop, previous))!;
      newChildren[newTop] = nc;
      previous = nc;
      newTop++;
      oldTop++;
    }
    if (haveOld) {
      for (const bucket of keyed.values()) {
        for (const e of bucket) if (forgottenChildren === null || !forgottenChildren.has(e)) this.deactivateChild(e);
      }
    }
    return newChildren;
  }

  /** 递归更新子树里 RenderObjectElement 的 slot。 */
  updateSlotForChild(child: Element, newSlot: unknown): void {
    const visit = (e: Element): void => {
      e.updateSlot(newSlot);
      if (!(e instanceof RenderObjectElement)) e.visitChildren(visit);
    };
    visit(child);
  }

  /** @internal 自身或第一个子孙 RenderObjectElement。 */
  _descendantRenderObjectElement(): RenderObjectElement | null {
    let e: Element | null = this;
    const holder: { next: Element | null } = { next: null };
    const grab = (c: Element): void => {
      holder.next = c;
    };
    while (e !== null) {
      if (e instanceof RenderObjectElement) return e;
      holder.next = null;
      e.visitChildren(grab);
      e = holder.next;
    }
    return null;
  }

  /** 修改本 Element 的 slot。 */
  updateSlot(newSlot: unknown): void {
    this.slot = newSlot;
  }

  /** 从渲染树分离(停用时)。 */
  detachRenderObject(): void {
    this.visitChildren((c) => c.detachRenderObject());
    this.slot = null;
  }

  /** 接回渲染树(GlobalKey 复用时)。 */
  attachRenderObject(newSlot: unknown): void {
    this.visitChildren((c) => c.attachRenderObject(newSlot));
    this.slot = newSlot;
  }

  /** 由 view 创建并挂载子 Element(GlobalKey 可复用停用中的 Element)。 */
  inflateView(newView: View, newSlot: unknown): Element {
    const key = newView.key;
    if (key instanceof GlobalKey) {
      const reused = this.#retakeInactiveElement(key, newView);
      if (reused !== null) {
        reused._activateWithParent(this, newSlot);
        const updated = this.updateChild(reused, newView, newSlot);
        return updated!;
      }
    }
    const child = newView.createElement();
    child.mount(this, newSlot);
    return child;
  }

  #retakeInactiveElement(key: GlobalKey, newView: View): Element | null {
    const element = key.currentElement;
    if (element === null || !View.canUpdate(element.view, newView)) return null;
    const parent = element.parent;
    if (parent !== null) {
      // 从旧父节点处"偷走"(同帧内移动)。
      parent.forgetChild(element);
      parent.deactivateChild(element);
    }
    this.owner?._inactiveElements.delete(element);
    return element;
  }

  /** @internal */
  _activateWithParent(parent: Element, newSlot: unknown): void {
    this.parent = parent;
    const updateDepth = (e: Element, d: number): void => {
      e.depth = d;
      e.visitChildren((c) => updateDepth(c, d + 1));
    };
    updateDepth(this, parent.depth + 1);
    const activateRecursively = (e: Element): void => {
      e.activate();
      e.visitChildren(activateRecursively);
    };
    activateRecursively(this);
    this.attachRenderObject(newSlot);
  }

  /** 父节点忘记某个子节点(其已被 GlobalKey 移走)。 */
  forgetChild(_child: Element): void {}

  /** 停用子节点(放入 inactive 集合, 帧尾卸载)。 */
  deactivateChild(child: Element): void {
    child.parent = null;
    child.detachRenderObject();
    this.owner?._deactivate(child);
  }

  /** 从 inactive 恢复。 */
  activate(): void {
    const hadDependencies = (this._dependencies !== null && this._dependencies.size > 0) || this.#hadUnsatisfiedDependencies;
    this.#lifecycle = "active";
    this._dependencies?.clear();
    this.#hadUnsatisfiedDependencies = false;
    this._updateInheritance();
    if (this.dirty) this.owner?.scheduleBuildFor(this);
    if (hadDependencies) this.didChangeDependencies();
  }

  /** 停用(从树中移除, 可能在本帧内被重新插入)。 */
  deactivate(): void {
    if (this._dependencies !== null) {
      for (const dep of this._dependencies) dep._removeDependent(this);
    }
    this._inheritedElements = null;
    this.#lifecycle = "inactive";
  }

  /** 永久卸载。 */
  unmount(): void {
    const key = this.#view?.key;
    if (key instanceof GlobalKey && globalKeyRegistry.get(key) === this) globalKeyRegistry.delete(key);
    this._dependencies = null;
    this.#view = null;
    this.#lifecycle = "defunct";
  }

  /** 依赖的 InheritedView 变化。 */
  didChangeDependencies(): void {
    this.markNeedsBuild();
  }

  /** 标记需要重建(下一帧)。 */
  markNeedsBuild(): void {
    if (this.#lifecycle !== "active") return;
    if (this.dirty) return;
    this.dirty = true;
    this.owner?.scheduleBuildFor(this);
  }

  /** 若 dirty(或 force)则重建。 */
  rebuild(force = false): void {
    if (this.#lifecycle !== "active" || (!this.dirty && !force)) return;
    this.performRebuild();
  }

  /** 执行重建(子类实现)。 */
  protected abstract performRebuild(): void;

  // ---------------------------------------------------------------------------
  // BuildContext

  /** 注册对 InheritedView 的依赖并返回之(其更新时本 Element 重建)。 */
  dependOnInheritedViewOfExactType<T extends InheritedView>(type: Type<T>): T | null {
    const ancestor = this._inheritedElements?.get(type) ?? null;
    if (ancestor !== null) return this.dependOnInheritedElement(ancestor) as T;
    this.#hadUnsatisfiedDependencies = true;
    return null;
  }

  /** 注册对指定 InheritedElement 的依赖。 */
  dependOnInheritedElement(ancestor: InheritedElement): InheritedView {
    if (this._dependencies === null) this._dependencies = new Set();
    this._dependencies.add(ancestor);
    ancestor._addDependent(this);
    return ancestor.view as InheritedView;
  }

  /** 查找 InheritedView(不注册依赖)。 */
  getInheritedViewOfExactType<T extends InheritedView>(type: Type<T>): T | null {
    return (this._inheritedElements?.get(type)?.view as T | undefined) ?? null;
  }

  /** 查找 InheritedElement(不注册依赖)。 */
  getElementForInheritedViewOfExactType(type: Type<InheritedView>): InheritedElement | null {
    return this._inheritedElements?.get(type) ?? null;
  }

  /** 向上遍历祖先, visitor 返回 false 停止。 */
  visitAncestorElements(visitor: (e: Element) => boolean): void {
    let a = this.parent;
    while (a !== null && visitor(a)) a = a.parent;
  }

  /** 最近的指定类型祖先 view。 */
  findAncestorViewOfExactType<T extends View>(type: Type<T>): T | null {
    let a = this.parent;
    while (a !== null) {
      if (a.view.constructor === type) return a.view as T;
      a = a.parent;
    }
    return null;
  }

  /** 最近的指定类型(含子类)祖先 State。 */
  findAncestorStateOfType<T extends State>(type: Type<T>): T | null {
    let a = this.parent;
    while (a !== null) {
      if (a instanceof StatefulElement && a.state instanceof type) return a.state as T;
      a = a.parent;
    }
    return null;
  }

  /** 最远的指定类型祖先 State。 */
  findRootAncestorStateOfType<T extends State>(type: Type<T>): T | null {
    let a = this.parent;
    let found: T | null = null;
    while (a !== null) {
      if (a instanceof StatefulElement && a.state instanceof type) found = a.state as T;
      a = a.parent;
    }
    return found;
  }

  /** 最近的祖先 RenderObject。 */
  findAncestorRenderObjectOfType<T extends RenderObject>(type: Type<T>): T | null {
    let a = this.parent;
    while (a !== null) {
      if (a instanceof RenderObjectElement && a.renderObject instanceof type) return a.renderObject as T;
      a = a.parent;
    }
    return null;
  }

  /** 自身或子孙的第一个 RenderObject。 */
  get renderObject(): RenderObject | null {
    return this._descendantRenderObjectElement()?.renderObject ?? null;
  }

  findRenderObject(): RenderObject | null {
    return this.renderObject;
  }

  /** RenderBox 的尺寸(布局后)。 */
  get size(): Size | null {
    const ro = this.renderObject;
    return ro instanceof RenderBox && ro.hasSize ? ro.size : null;
  }

  toStringShort(): string {
    return this.#view !== null ? this.#view.toStringShort() : describeIdentity(this);
  }

  /** 调试:子树描述。 */
  toStringDeep(indent = ""): string {
    let s = `${indent}${this.toStringShort()}\n`;
    this.visitChildren((c) => {
      s += c.toStringDeep(indent + "  ");
    });
    return s;
  }
}

/** 组合型 Element 基类(只有一个子 Element, 由 build 产生)。 */
export abstract class ComponentElement extends Element {
  #child: Element | null = null;

  get child(): Element | null {
    return this.#child;
  }

  override mount(parent: Element | null, newSlot: unknown): void {
    super.mount(parent, newSlot);
    this.firstBuild();
  }

  protected firstBuild(): void {
    this.rebuild();
  }

  protected performRebuild(): void {
    let built: View;
    try {
      built = this.build();
    } catch (e) {
      reportError(e, `building ${this.toStringShort()}`);
      built = new ErrorView(e);
    } finally {
      this.dirty = false;
    }
    try {
      this.#child = this.updateChild(this.#child, built, this.slot);
    } catch (e) {
      reportError(e, `building ${this.toStringShort()}`);
      this.#child = this.updateChild(null, new ErrorView(e), this.slot);
    }
  }

  /** 产生子 view。 */
  protected abstract build(): View;

  override visitChildren(visitor: (child: Element) => void): void {
    if (this.#child !== null) visitor(this.#child);
  }

  override forgetChild(child: Element): void {
    if (child === this.#child) this.#child = null;
  }
}

/** 无状态组件。 */
export abstract class StatelessView extends View {
  createElement(): Element {
    return new StatelessElement(this);
  }
  /** 构建子树。 */
  abstract build(context: BuildContext): View;
}

export class StatelessElement extends ComponentElement {
  protected build(): View {
    return (this.view as StatelessView).build(this);
  }
  override update(newView: View): void {
    super.update(newView);
    this.rebuild(true);
  }
}

/** 有状态组件:可变状态保存在 {@link State} 中。 */
export abstract class StatefulView extends View {
  createElement(): Element {
    return new StatefulElement(this);
  }
  /** 创建状态对象(每个 Element 一次)。 */
  abstract createState(): State;
}

/**
 * StatefulView 的可变状态。也是 TickerProvider(便于 `new AnimationController({ vsync: this })`),
 * 卸载时会释放仍存活的 Ticker。
 */
export abstract class State<W extends StatefulView = StatefulView> implements TickerProvider {
  /** @internal */ _view: W | null = null;
  /** @internal */ _element: StatefulElement | null = null;
  #tickers: Set<Ticker> | null = null;

  /** 当前 view 配置。 */
  get view(): W {
    if (this._view === null) throw new MomoError("State.view accessed before initState");
    return this._view;
  }
  /** 对应的 BuildContext。 */
  get context(): BuildContext {
    if (this._element === null) throw new MomoError("State.context accessed after dispose");
    return this._element;
  }
  get mounted(): boolean {
    return this._element !== null;
  }

  /** 插入树时调用一次。 */
  initState(): void {}
  /** view 被同类型新 view 替换后调用。 */
  didUpdateView(_oldView: W): void {}
  /** 依赖的 InheritedView 变化(及 initState 之后)调用。 */
  didChangeDependencies(): void {}
  /** 从树中移除(可能被重新插入)。 */
  deactivate(): void {}
  /** 被重新插入。 */
  activate(): void {}
  /** 永久移除。 */
  dispose(): void {}

  /** 修改状态并安排重建。 */
  setState(fn?: () => void): void {
    if (this._element === null) throw new MomoError(`setState() called after dispose(): ${this.constructor.name}`);
    fn?.();
    this._element.markNeedsBuild();
  }

  /** 构建 UI。 */
  abstract build(context: BuildContext): View;

  /** TickerProvider:创建与本 State 生命周期绑定的 Ticker。 */
  createTicker(onTick: TickerCallback): Ticker {
    const t = new Ticker(onTick, this.constructor.name);
    const tickers = (this.#tickers ??= new Set());
    tickers.add(t);
    // 单个 Ticker 提前 dispose(如 AnimationController.dispose / resync)时立即移出,
    // 否则长寿 State 会一直持有已释放的 Ticker 及其回调闭包(随使用次数线性增长)。
    t._onDispose = (ticker) => {
      this.#tickers?.delete(ticker);
    };
    return t;
  }

  /** @internal */
  _disposeTickers(): void {
    const tickers = this.#tickers;
    if (tickers === null) return;
    this.#tickers = null;
    for (const t of tickers) t.dispose();
  }

  /** @internal 测试/诊断用:当前持有的 Ticker 数。 */
  get debugTickerCount(): number {
    return this.#tickers?.size ?? 0;
  }
}

export class StatefulElement extends ComponentElement {
  readonly state: State;
  #didChangeDependencies = false;

  constructor(view: StatefulView) {
    super(view);
    this.state = view.createState();
    this.state._element = this;
    this.state._view = view;
  }

  protected build(): View {
    return this.state.build(this);
  }

  protected override firstBuild(): void {
    try {
      this.state.initState();
      this.state.didChangeDependencies();
    } catch (e) {
      reportError(e, `in initState of ${this.state.constructor.name}`);
    }
    super.firstBuild();
  }

  protected override performRebuild(): void {
    if (this.#didChangeDependencies) {
      this.#didChangeDependencies = false;
      try {
        this.state.didChangeDependencies();
      } catch (e) {
        reportError(e, `in didChangeDependencies of ${this.state.constructor.name}`);
      }
    }
    super.performRebuild();
  }

  override update(newView: View): void {
    super.update(newView);
    const old = this.state._view!;
    this.state._view = newView as StatefulView;
    try {
      this.state.didUpdateView(old);
    } catch (e) {
      reportError(e, `in didUpdateView of ${this.state.constructor.name}`);
    }
    this.rebuild(true);
  }

  override activate(): void {
    super.activate();
    this.state.activate();
    this.markNeedsBuild();
  }

  override deactivate(): void {
    this.state.deactivate();
    super.deactivate();
  }

  override unmount(): void {
    super.unmount();
    try {
      this.state.dispose();
    } catch (e) {
      reportError(e, `in dispose of ${this.state.constructor.name}`);
    }
    this.state._disposeTickers();
    this.state._element = null;
  }

  override didChangeDependencies(): void {
    super.didChangeDependencies();
    this.#didChangeDependencies = true;
  }
}

/** 只包装一个子 view 的组件基类。 */
export abstract class ProxyView extends View {
  readonly child: View;
  constructor(o: { key?: Key | null; child: View }) {
    super(o.key);
    this.child = o.child;
  }
}

export abstract class ProxyElement extends ComponentElement {
  protected build(): View {
    return (this.view as ProxyView).child;
  }
  override update(newView: View): void {
    const old = this.view as ProxyView;
    super.update(newView);
    this.updated(old);
    this.rebuild(true);
  }
  /** view 更新后调用。 */
  protected updated(oldView: ProxyView): void {
    this.notifyClients(oldView);
  }
  /** 通知依赖者。 */
  protected abstract notifyClients(oldView: ProxyView): void;
}

/**
 * 向子树提供数据;依赖者(dependOnInheritedViewOfExactType)在 updateShouldNotify 为 true 时精准重建。
 */
export abstract class InheritedView extends ProxyView {
  createElement(): Element {
    return new InheritedElement(this);
  }
  /** 新旧 view 替换时是否通知依赖者。 */
  abstract updateShouldNotify(oldView: InheritedView): boolean;
}

export class InheritedElement extends ProxyElement {
  readonly #dependents = new Set<Element>();

  override _updateInheritance(): void {
    const incoming = this.parent?._inheritedElements ?? null;
    const map = incoming !== null ? new Map(incoming) : new Map<Function, InheritedElement>();
    map.set(this.view.constructor, this);
    this._inheritedElements = map;
  }

  /** @internal */
  _addDependent(e: Element): void {
    this.#dependents.add(e);
  }
  /** @internal */
  _removeDependent(e: Element): void {
    this.#dependents.delete(e);
  }
  /** 依赖者数量(测试)。 */
  get dependentCount(): number {
    return this.#dependents.size;
  }

  protected override updated(oldView: ProxyView): void {
    if ((this.view as InheritedView).updateShouldNotify(oldView as InheritedView)) super.updated(oldView);
  }

  protected notifyClients(_oldView: ProxyView): void {
    for (const d of [...this.#dependents]) {
      if (d.lifecycle === "active") d.didChangeDependencies();
    }
  }
}

/** 为子 RenderObject 写 parentData 的组件(Expanded、Positioned)。 */
export abstract class ParentDataView extends ProxyView {
  createElement(): Element {
    return new ParentDataElement(this);
  }
  /** 把参数写入 renderObject.parentData。 */
  abstract applyParentData(renderObject: RenderObject): void;
}

export class ParentDataElement extends ProxyElement {
  /** 对最近的子孙 RenderObject 应用 parentData。 */
  applyParentData(view: ParentDataView): void {
    const apply = (e: Element): void => {
      if (e instanceof RenderObjectElement) {
        view.applyParentData(e.renderObject);
      } else {
        e.visitChildren(apply);
      }
    };
    this.visitChildren(apply);
  }
  protected notifyClients(_oldView: ProxyView): void {
    this.applyParentData(this.view as ParentDataView);
  }
}

/** 配置 RenderObject 的 view。 */
export abstract class RenderObjectView extends View {
  /** 创建渲染对象。 */
  abstract createRenderObject(context: BuildContext): RenderObject;
  /** 用当前配置更新渲染对象。 */
  updateRenderObject(_context: BuildContext, _renderObject: RenderObject): void {}
  /** 渲染对象被卸载。 */
  didUnmountRenderObject(_renderObject: RenderObject): void {}
}

/** RenderObjectView 的 Element:管理 RenderObject 在渲染树中的插入/移动/移除。 */
export abstract class RenderObjectElement extends Element {
  #renderObject: RenderObject | null = null;
  #ancestor: RenderObjectElement | null = null;

  override get renderObject(): RenderObject {
    if (this.#renderObject === null) throw new MomoError(`${this.toStringShort()} has no render object`);
    return this.#renderObject;
  }

  override mount(parent: Element | null, newSlot: unknown): void {
    super.mount(parent, newSlot);
    this.#renderObject = (this.view as RenderObjectView).createRenderObject(this);
    this.attachRenderObject(newSlot);
    this.dirty = false;
  }

  override update(newView: View): void {
    super.update(newView);
    (this.view as RenderObjectView).updateRenderObject(this, this.renderObject);
    this.dirty = false;
  }

  protected performRebuild(): void {
    (this.view as RenderObjectView).updateRenderObject(this, this.renderObject);
    this.dirty = false;
  }

  #findAncestorRenderObjectElement(): RenderObjectElement | null {
    let a = this.parent;
    while (a !== null && !(a instanceof RenderObjectElement)) a = a.parent;
    return a;
  }

  #findAncestorParentDataElement(): ParentDataElement | null {
    let a = this.parent;
    while (a !== null && !(a instanceof RenderObjectElement)) {
      if (a instanceof ParentDataElement) return a;
      a = a.parent;
    }
    return null;
  }

  override attachRenderObject(newSlot: unknown): void {
    this.slot = newSlot;
    this.#ancestor = this.#findAncestorRenderObjectElement();
    this.#ancestor?.insertRenderObjectChild(this.renderObject, newSlot);
    const pde = this.#findAncestorParentDataElement();
    if (pde !== null) (pde.view as ParentDataView).applyParentData(this.renderObject);
  }

  override detachRenderObject(): void {
    if (this.#ancestor !== null) {
      this.#ancestor.removeRenderObjectChild(this.renderObject, this.slot);
      this.#ancestor = null;
    }
    this.slot = null;
  }

  override updateSlot(newSlot: unknown): void {
    const old = this.slot;
    this.slot = newSlot;
    this.#ancestor?.moveRenderObjectChild(this.renderObject, old, newSlot);
  }

  override unmount(): void {
    const view = this.view as RenderObjectView;
    super.unmount();
    const ro = this.#renderObject;
    if (ro !== null) {
      view.didUnmountRenderObject(ro);
      ro.dispose();
    }
  }

  /** 插入子渲染对象。 */
  abstract insertRenderObjectChild(child: RenderObject, slot: unknown): void;
  /** 移动子渲染对象。 */
  abstract moveRenderObjectChild(child: RenderObject, oldSlot: unknown, newSlot: unknown): void;
  /** 移除子渲染对象。 */
  abstract removeRenderObjectChild(child: RenderObject, slot: unknown): void;
}

/** 无子节点的 RenderObjectView。 */
export abstract class LeafRenderObjectView extends RenderObjectView {
  createElement(): Element {
    return new LeafRenderObjectElement(this);
  }
}

export class LeafRenderObjectElement extends RenderObjectElement {
  insertRenderObjectChild(): void {
    throw new MomoError("LeafRenderObjectElement cannot have children");
  }
  moveRenderObjectChild(): void {
    throw new MomoError("LeafRenderObjectElement cannot have children");
  }
  removeRenderObjectChild(): void {
    throw new MomoError("LeafRenderObjectElement cannot have children");
  }
}

/** 单子节点 RenderObject 的接口。 */
interface RenderObjectWithChild {
  child: RenderObject | null;
}

function hasChildSlot(o: RenderObject): o is RenderObject & RenderObjectWithChild {
  return "child" in o;
}

/** 单子节点的 RenderObjectView。 */
export abstract class SingleChildRenderObjectView extends RenderObjectView {
  readonly child: View | null;
  constructor(o: { key?: Key | null; child?: View | null } = {}) {
    super(o.key);
    this.child = o.child ?? null;
  }
  createElement(): Element {
    return new SingleChildRenderObjectElement(this);
  }
}

export class SingleChildRenderObjectElement extends RenderObjectElement {
  #child: Element | null = null;

  override visitChildren(visitor: (child: Element) => void): void {
    if (this.#child !== null) visitor(this.#child);
  }
  override forgetChild(child: Element): void {
    if (child === this.#child) this.#child = null;
  }
  override mount(parent: Element | null, newSlot: unknown): void {
    super.mount(parent, newSlot);
    this.#child = this.updateChild(this.#child, (this.view as SingleChildRenderObjectView).child, null);
  }
  override update(newView: View): void {
    super.update(newView);
    this.#child = this.updateChild(this.#child, (this.view as SingleChildRenderObjectView).child, null);
  }
  insertRenderObjectChild(child: RenderObject, _slot: unknown): void {
    const ro = this.renderObject;
    if (!hasChildSlot(ro)) throw new MomoError(`${ro.toStringShort()} does not accept a child`);
    ro.child = child;
  }
  moveRenderObjectChild(): void {}
  removeRenderObjectChild(child: RenderObject, _slot: unknown): void {
    const ro = this.renderObject;
    if (hasChildSlot(ro) && ro.child === child) ro.child = null;
  }
}

/** 多子节点的 RenderObjectView(渲染对象须为 ContainerRenderBox)。 */
export abstract class MultiChildRenderObjectView extends RenderObjectView {
  readonly children: readonly View[];
  constructor(o: { key?: Key | null; children?: readonly View[] } = {}) {
    super(o.key);
    this.children = o.children ?? [];
  }
  createElement(): Element {
    return new MultiChildRenderObjectElement(this);
  }
}

export class MultiChildRenderObjectElement extends RenderObjectElement {
  #children: Element[] = [];
  #forgotten = new Set<Element>();

  /** 子 Element(不含已被 GlobalKey 移走的)。 */
  get children(): readonly Element[] {
    return this.#children.filter((c) => !this.#forgotten.has(c));
  }

  override visitChildren(visitor: (child: Element) => void): void {
    for (const c of this.#children) if (!this.#forgotten.has(c)) visitor(c);
  }
  override forgetChild(child: Element): void {
    this.#forgotten.add(child);
  }
  #container(): ContainerRenderBox {
    const ro = this.renderObject;
    if (!(ro instanceof ContainerRenderBox)) throw new MomoError(`${ro.toStringShort()} is not a ContainerRenderBox`);
    return ro;
  }
  insertRenderObjectChild(child: RenderObject, slot: unknown): void {
    const after = slot instanceof IndexedSlot ? (slot.value?.renderObject ?? null) : null;
    this.#container().insert(child as RenderBox, after as RenderBox | null);
  }
  moveRenderObjectChild(child: RenderObject, _oldSlot: unknown, newSlot: unknown): void {
    const after = newSlot instanceof IndexedSlot ? (newSlot.value?.renderObject ?? null) : null;
    this.#container().move(child as RenderBox, after as RenderBox | null);
  }
  removeRenderObjectChild(child: RenderObject, _slot: unknown): void {
    this.#container().remove(child as RenderBox);
  }
  override mount(parent: Element | null, newSlot: unknown): void {
    super.mount(parent, newSlot);
    const views = (this.view as MultiChildRenderObjectView).children;
    const children: Element[] = [];
    let previous: Element | null = null;
    for (let i = 0; i < views.length; i++) {
      const c = this.inflateView(views[i]!, new IndexedSlot(i, previous));
      children.push(c);
      previous = c;
    }
    this.#children = children;
  }
  override update(newView: View): void {
    super.update(newView);
    this.#children = this.updateChildren(this.#children, (this.view as MultiChildRenderObjectView).children, this.#forgotten);
    this.#forgotten.clear();
  }
}

/**
 * 构建调度:维护 dirty Element 列表, buildScope 中按深度排序重建;帧尾卸载 inactive Element。
 */
export class BuildOwner {
  /** 第一个 Element 变脏时调用(请求帧)。 */
  onBuildScheduled: (() => void) | null;
  #dirty: Element[] = [];
  #needsResort = false;
  #scheduledFlush = false;
  #building = false;
  /** @internal */ readonly _inactiveElements = new Set<Element>();

  constructor(onBuildScheduled: (() => void) | null = null) {
    this.onBuildScheduled = onBuildScheduled;
  }

  /** 是否处于 buildScope 中。 */
  get debugBuilding(): boolean {
    return this.#building;
  }
  /** 待重建的 Element 数。 */
  get dirtyCount(): number {
    return this.#dirty.length;
  }

  scheduleBuildFor(element: Element): void {
    if (element.inDirtyList) {
      this.#needsResort = true;
      return;
    }
    if (!this.#scheduledFlush && !this.#building) {
      this.#scheduledFlush = true;
      this.onBuildScheduled?.();
    }
    this.#dirty.push(element);
    element.inDirtyList = true;
  }

  /** 重建所有 dirty Element(可先执行 callback)。 */
  buildScope(_context: Element | null, callback?: () => void): void {
    if (callback === undefined && this.#dirty.length === 0) return;
    const wasBuilding = this.#building;
    this.#building = true;
    try {
      this.#scheduledFlush = true;
      callback?.();
      const sort = (a: Element, b: Element): number => a.depth - b.depth || (a.dirty === b.dirty ? 0 : a.dirty ? -1 : 1);
      this.#dirty.sort(sort);
      this.#needsResort = false;
      let count = this.#dirty.length;
      let index = 0;
      while (index < count) {
        const element = this.#dirty[index]!;
        try {
          element.rebuild();
        } catch (e) {
          reportError(e, `while rebuilding ${element.toStringShort()}`);
        }
        index++;
        if (count < this.#dirty.length || this.#needsResort) {
          this.#dirty.sort(sort);
          this.#needsResort = false;
          count = this.#dirty.length;
          while (index > 0 && this.#dirty[index - 1]!.dirty) index--;
        }
      }
    } finally {
      for (const e of this.#dirty) e.inDirtyList = false;
      this.#dirty = [];
      this.#scheduledFlush = false;
      this.#building = wasBuilding;
    }
  }

  /** @internal 停用子树。 */
  _deactivate(element: Element): void {
    const recurse = (e: Element): void => {
      e.deactivate();
      e.visitChildren(recurse);
    };
    recurse(element);
    this._inactiveElements.add(element);
  }

  /** 卸载本帧未被复用的 inactive Element。 */
  finalizeTree(): void {
    const elements = [...this._inactiveElements];
    this._inactiveElements.clear();
    elements.sort((a, b) => b.depth - a.depth);
    const unmount = (e: Element): void => {
      e.visitChildren(unmount);
      e.unmount();
    };
    for (const e of elements) {
      try {
        unmount(e);
      } catch (err) {
        reportError(err, "while finalizing the view tree");
      }
    }
  }
}

/** 构建出错时显示的占位 view(红底)。 */
export class ErrorView extends LeafRenderObjectView {
  readonly message: string;
  constructor(error: unknown) {
    super();
    this.message = error instanceof Error ? error.message : String(error);
  }
  createRenderObject(): RenderObject {
    return new RenderErrorBox(this.message);
  }
}


const errorPaint = new Paint();
errorPaint.color = 0xffd32f2f;

class RenderErrorBox extends RenderBox {
  constructor(readonly message: string) {
    super();
  }
  override performLayout(): void {
    const b = this.constraints.biggest;
    this.size = Number.isFinite(b.width) && Number.isFinite(b.height) ? b : this.constraints.smallest;
  }
  override paint(context: PaintingContext, offset: Offset): void {
    context.canvas.drawRect(offset.dx, offset.dy, this.size.width, this.size.height, errorPaint);
  }
}
