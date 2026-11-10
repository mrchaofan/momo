import { ObjectKey, type Key } from "../foundation/key";
import { Stack } from "./basic";
import { InheritedWidget, State, StatefulWidget, type BuildContext, type Widget } from "./framework";

/** 浮层条目。 */
export class OverlayEntry {
  readonly builder: (context: BuildContext) => Widget;
  /** 是否完全遮住下层(下层可不绘制;本实现仅作标记)。 */
  readonly opaque: boolean;
  #overlay: OverlayState | null = null;
  #state: OverlayEntryState | null = null;

  constructor(o: { builder: (context: BuildContext) => Widget; opaque?: boolean }) {
    this.builder = o.builder;
    this.opaque = o.opaque ?? false;
  }

  /** 是否已插入。 */
  get mounted(): boolean {
    return this.#overlay !== null;
  }

  /** 从 Overlay 移除。 */
  remove(): void {
    const o = this.#overlay;
    if (o === null) return;
    this.#overlay = null;
    o._remove(this);
  }

  /** 让 builder 重新执行。 */
  markNeedsBuild(): void {
    this.#state?.rebuild();
  }

  /** @internal */
  _attach(o: OverlayState | null): void {
    this.#overlay = o;
  }
  /** @internal */
  _setState(s: OverlayEntryState | null): void {
    this.#state = s;
  }
}

class OverlayEntryWidget extends StatefulWidget {
  constructor(readonly entry: OverlayEntry) {
    super(new ObjectKey(entry));
  }
  createState(): State {
    return new OverlayEntryState();
  }
}

class OverlayEntryState extends State<OverlayEntryWidget> {
  override initState(): void {
    this.widget.entry._setState(this);
  }
  override dispose(): void {
    this.widget.entry._setState(null);
  }
  rebuild(): void {
    if (this.mounted) this.setState();
  }
  build(context: BuildContext): Widget {
    return this.widget.entry.builder(context);
  }
}

class OverlayScope extends InheritedWidget {
  constructor(readonly overlay: OverlayState, child: Widget) {
    super({ child });
  }
  updateShouldNotify(old: OverlayScope): boolean {
    return old.overlay !== this.overlay;
  }
}

/** 浮层栈(对话框、菜单、提示等)。 */
export class Overlay extends StatefulWidget {
  readonly initialEntries: readonly OverlayEntry[];
  constructor(o: { key?: Key | null; initialEntries?: readonly OverlayEntry[] } = {}) {
    super(o.key);
    this.initialEntries = o.initialEntries ?? [];
  }
  createState(): State {
    return new OverlayState();
  }
  static maybeOf(context: BuildContext): OverlayState | null {
    return context.dependOnInheritedWidgetOfExactType(OverlayScope)?.overlay ?? null;
  }
  static of(context: BuildContext): OverlayState {
    const o = Overlay.maybeOf(context);
    if (o === null) throw new Error("No Overlay widget found in context");
    return o;
  }
}

export class OverlayState extends State<Overlay> {
  readonly #entries: OverlayEntry[] = [];

  get entries(): readonly OverlayEntry[] {
    return this.#entries;
  }

  override initState(): void {
    for (const e of this.widget.initialEntries) {
      e._attach(this);
      this.#entries.push(e);
    }
  }

  /** 插入到顶部(或 above 之上)。 */
  insert(entry: OverlayEntry, o: { above?: OverlayEntry } = {}): void {
    entry._attach(this);
    const i = o.above !== undefined ? this.#entries.indexOf(o.above) : -1;
    if (i >= 0) this.#entries.splice(i + 1, 0, entry);
    else this.#entries.push(entry);
    this.setState();
  }

  /** @internal */
  _remove(entry: OverlayEntry): void {
    const i = this.#entries.indexOf(entry);
    if (i >= 0) {
      this.#entries.splice(i, 1);
      if (this.mounted) this.setState();
    }
  }

  build(_context: BuildContext): Widget {
    return new OverlayScope(this, new Stack({
      fit: "expand",
      children: this.#entries.map((e) => new OverlayEntryWidget(e)),
    }));
  }
}
