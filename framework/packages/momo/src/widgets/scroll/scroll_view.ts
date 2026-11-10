import type { Key } from "../../foundation/key";
import { EdgeInsets } from "../../painting/edge_insets";
import type { Axis, Clip } from "../../painting/geometry";
import type { RenderBox } from "../../rendering/box";
import type { RenderObject } from "../../rendering/object";
import { RenderListViewport, RenderSingleChildViewport, type ListChildManager } from "../../rendering/viewport";
import { Padding, RepaintBoundary } from "../basic";
import {
  Element, RenderObjectElement, RenderObjectWidget, SingleChildRenderObjectWidget, StatelessWidget, type BuildContext, type Widget,
} from "../framework";
import { Scrollable } from "./scrollable";
import type { ScrollController, ScrollPosition } from "./scroll_position";

class SingleChildViewport extends SingleChildRenderObjectWidget {
  constructor(readonly axis: Axis, readonly offset: ScrollPosition, readonly clipBehavior: Clip, child: Widget | null) {
    super({ child });
  }
  createRenderObject(): RenderSingleChildViewport {
    return new RenderSingleChildViewport({ axis: this.axis, offset: this.offset, clipBehavior: this.clipBehavior });
  }
  override updateRenderObject(_c: BuildContext, r: RenderSingleChildViewport): void {
    r.axis = this.axis;
    r.offset = this.offset;
    r.clipBehavior = this.clipBehavior;
  }
}

/** 单子节点滚动容器。 */
export class SingleChildScrollView extends StatelessWidget {
  readonly scrollDirection: Axis;
  readonly controller: ScrollController | null;
  readonly padding: EdgeInsets | null;
  readonly clipBehavior: Clip;
  readonly child: Widget | null;
  constructor(o: { key?: Key | null; scrollDirection?: Axis; controller?: ScrollController | null; padding?: EdgeInsets | null; clipBehavior?: Clip; child?: Widget | null } = {}) {
    super(o.key);
    this.scrollDirection = o.scrollDirection ?? "vertical";
    this.controller = o.controller ?? null;
    this.padding = o.padding ?? null;
    this.clipBehavior = o.clipBehavior ?? "hardEdge";
    this.child = o.child ?? null;
  }
  build(_context: BuildContext): Widget {
    const content = this.padding !== null && this.child !== null ? new Padding({ padding: this.padding, child: this.child }) : this.child;
    return new Scrollable({
      axis: this.scrollDirection,
      controller: this.controller,
      viewportBuilder: (_ctx, position) => new SingleChildViewport(this.scrollDirection, position, this.clipBehavior, content),
    });
  }
}

/** 列表项构建器;返回 null 表示列表结束(itemCount 为 null 时)。 */
export type IndexedWidgetBuilder = (context: BuildContext, index: number) => Widget | null;

class ListViewport extends RenderObjectWidget {
  constructor(
    readonly axis: Axis,
    readonly offset: ScrollPosition,
    readonly itemExtent: number | null,
    readonly itemCount: number | null,
    readonly itemBuilder: IndexedWidgetBuilder,
    readonly cacheExtent: number,
    readonly clipBehavior: Clip,
    readonly addRepaintBoundaries: boolean,
  ) {
    super();
  }
  createElement(): Element {
    return new ListViewportElement(this);
  }
  createRenderObject(): RenderListViewport {
    return new RenderListViewport({ axis: this.axis, offset: this.offset, itemExtent: this.itemExtent, cacheExtent: this.cacheExtent, clipBehavior: this.clipBehavior });
  }
  override updateRenderObject(_c: BuildContext, r: RenderListViewport): void {
    r.axis = this.axis;
    r.offset = this.offset;
    r.itemExtent = this.itemExtent;
    r.cacheExtent = this.cacheExtent;
    r.clipBehavior = this.clipBehavior;
  }
}

/** 懒加载列表的 Element:作为 RenderListViewport 的 ListChildManager, 在布局期按需构建/回收子项。 */
export class ListViewportElement extends RenderObjectElement implements ListChildManager {
  #children = new Map<number, Element>();

  get #viewport(): RenderListViewport {
    return this.renderObject as RenderListViewport;
  }
  get childCount(): number | null {
    return (this.widget as ListViewport).itemCount;
  }

  override mount(parent: Element | null, newSlot: unknown): void {
    super.mount(parent, newSlot);
    this.#viewport.childManager = this;
  }

  override unmount(): void {
    this.#viewport.childManager = null;
    super.unmount();
  }

  override update(newWidget: Widget): void {
    super.update(newWidget);
    // 数据可能变化:重建现有子项并重新测量。
    const count = this.childCount;
    for (const [index, el] of [...this.#children].sort((a, b) => a[0] - b[0])) {
      const w = count !== null && index >= count ? null : this.#build(index);
      const updated = this.updateChild(el, w, index);
      if (updated === null) this.#children.delete(index);
      else this.#children.set(index, updated);
    }
    this.#viewport.markChildrenDirty();
  }

  protected override performRebuild(): void {
    super.performRebuild();
  }

  #build(index: number): Widget | null {
    const w = this.widget as ListViewport;
    if (w.itemCount !== null && (index < 0 || index >= w.itemCount)) return null;
    const child = w.itemBuilder(this, index);
    if (child === null) return null;
    return w.addRepaintBoundaries ? new RepaintBoundary({ child }) : child;
  }

  createChild(index: number): void {
    this.owner?.buildScope(this, () => {
      const w = this.#build(index);
      const el = this.updateChild(this.#children.get(index) ?? null, w, index);
      if (el !== null) this.#children.set(index, el);
      else this.#children.delete(index);
    });
  }

  removeChild(index: number): void {
    this.owner?.buildScope(this, () => {
      const el = this.#children.get(index);
      if (el !== undefined) this.updateChild(el, null, index);
      this.#children.delete(index);
    });
  }

  override visitChildren(visitor: (child: Element) => void): void {
    for (const [, el] of [...this.#children].sort((a, b) => a[0] - b[0])) visitor(el);
  }

  override forgetChild(child: Element): void {
    for (const [i, el] of this.#children) if (el === child) this.#children.delete(i);
  }

  insertRenderObjectChild(child: RenderObject, slot: unknown): void {
    this.#viewport.insertIndexed(child as RenderBox, slot as number);
  }
  moveRenderObjectChild(child: RenderObject, _oldSlot: unknown, newSlot: unknown): void {
    this.#viewport.remove(child as RenderBox);
    this.#viewport.insertIndexed(child as RenderBox, newSlot as number);
  }
  removeRenderObjectChild(child: RenderObject, _slot: unknown): void {
    this.#viewport.remove(child as RenderBox);
  }
}

/** ListView 参数。 */
export interface ListViewOptions {
  key?: Key | null;
  scrollDirection?: Axis;
  controller?: ScrollController | null;
  padding?: EdgeInsets | null;
  itemExtent?: number | null;
  cacheExtent?: number;
  clipBehavior?: Clip;
  addRepaintBoundaries?: boolean;
}

/** 可滚动列表(子项懒构建, 只构建可见区域附近的项)。 */
export class ListView extends StatelessWidget {
  readonly scrollDirection: Axis;
  readonly controller: ScrollController | null;
  readonly padding: EdgeInsets | null;
  readonly itemExtent: number | null;
  readonly cacheExtent: number;
  readonly clipBehavior: Clip;
  readonly addRepaintBoundaries: boolean;
  readonly itemCount: number | null;
  readonly itemBuilder: IndexedWidgetBuilder;

  constructor(o: ListViewOptions & { children?: readonly Widget[] } = {}) {
    super(o.key);
    const children = o.children ?? [];
    this.scrollDirection = o.scrollDirection ?? "vertical";
    this.controller = o.controller ?? null;
    this.padding = o.padding ?? null;
    this.itemExtent = o.itemExtent ?? null;
    this.cacheExtent = o.cacheExtent ?? 250;
    this.clipBehavior = o.clipBehavior ?? "hardEdge";
    this.addRepaintBoundaries = o.addRepaintBoundaries ?? true;
    this.itemCount = children.length;
    this.itemBuilder = (_c, i) => children[i] ?? null;
  }

  /** 以 builder 构建子项。 */
  static builder(o: ListViewOptions & { itemBuilder: IndexedWidgetBuilder; itemCount?: number | null }): ListView {
    const lv = new ListView(o);
    const self = lv as { itemCount: number | null; itemBuilder: IndexedWidgetBuilder };
    self.itemCount = o.itemCount ?? null;
    self.itemBuilder = o.itemBuilder;
    return lv;
  }

  /** 带分隔项的 builder。 */
  static separated(o: ListViewOptions & { itemBuilder: IndexedWidgetBuilder; separatorBuilder: IndexedWidgetBuilder; itemCount: number }): ListView {
    return ListView.builder({
      ...o,
      itemCount: Math.max(0, o.itemCount * 2 - 1),
      itemBuilder: (c, i) => (i % 2 === 0 ? o.itemBuilder(c, i / 2) : o.separatorBuilder(c, (i - 1) / 2)),
    });
  }

  build(_context: BuildContext): Widget {
    const viewport = (position: ScrollPosition): Widget => new ListViewport(this.scrollDirection, position, this.itemExtent, this.itemCount,
      this.itemBuilder, this.cacheExtent, this.clipBehavior, this.addRepaintBoundaries);
    return new Scrollable({
      axis: this.scrollDirection,
      controller: this.controller,
      viewportBuilder: (_ctx, position) => this.padding !== null ? new Padding({ padding: this.padding, child: viewport(position) }) : viewport(position),
    });
  }
}
