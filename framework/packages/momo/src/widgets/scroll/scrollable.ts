import type { Key } from "../../foundation/key";
import { VerticalDragGestureRecognizer, HorizontalDragGestureRecognizer, type DragEndDetails, type DragUpdateDetails } from "../../gestures/drag";
import { GestureBinding } from "../../gestures/binding";
import { PointerScrollEvent, type PointerDeviceKind, type PointerSignalEvent } from "../../gestures/events";
import type { GestureRecognizer } from "../../gestures/recognizer";
import type { Axis } from "../../painting/geometry";
import { InheritedWidget, State, StatefulWidget, type BuildContext, type Type, type Widget } from "../framework";
import { Listener } from "../basic";
import { RawGestureDetector, gestureFactory, type GestureRecognizerFactory } from "../gesture_detector";
import { ScrollController, ScrollPosition } from "./scroll_position";

/** 视口构建器:由 Scrollable 提供 position。 */
export type ViewportBuilder = (context: BuildContext, position: ScrollPosition) => Widget;

/** 可拖动滚动的设备(桌面鼠标不拖动内容, 与 Flutter 桌面默认一致)。 */
export const kTouchLikeDeviceTypes: ReadonlySet<PointerDeviceKind> = new Set<PointerDeviceKind>(["touch", "stylus"]);

class ScrollableScope extends InheritedWidget {
  constructor(readonly scrollable: ScrollableState, readonly position: ScrollPosition, child: Widget) {
    super({ child });
  }
  updateShouldNotify(old: ScrollableScope): boolean {
    return old.position !== this.position;
  }
}

/** 处理滚动输入(滚轮、触摸拖动、惯性)并把 ScrollPosition 交给视口。 */
export class Scrollable extends StatefulWidget {
  readonly axis: Axis;
  readonly controller: ScrollController | null;
  readonly viewportBuilder: ViewportBuilder;
  readonly dragDevices: ReadonlySet<PointerDeviceKind>;
  constructor(o: { key?: Key | null; axis?: Axis; controller?: ScrollController | null; viewportBuilder: ViewportBuilder; dragDevices?: ReadonlySet<PointerDeviceKind> }) {
    super(o.key);
    this.axis = o.axis ?? "vertical";
    this.controller = o.controller ?? null;
    this.viewportBuilder = o.viewportBuilder;
    this.dragDevices = o.dragDevices ?? kTouchLikeDeviceTypes;
  }
  createState(): State {
    return new ScrollableState();
  }
  /** 最近的 Scrollable。 */
  static maybeOf(context: BuildContext): ScrollableState | null {
    return context.dependOnInheritedWidgetOfExactType(ScrollableScope)?.scrollable ?? null;
  }
}

export class ScrollableState extends State<Scrollable> {
  #fallbackController: ScrollController | null = null;
  #position: ScrollPosition | null = null;

  get position(): ScrollPosition {
    return this.#position!;
  }
  get #controller(): ScrollController {
    return this.widget.controller ?? (this.#fallbackController ??= new ScrollController());
  }

  override initState(): void {
    this.#createPosition();
  }

  #createPosition(): void {
    const old = this.#position;
    const p = this.#controller.createScrollPosition(this, this.widget.axis);
    if (old !== null) {
      p.applyViewportDimension(old.viewportDimension);
      p.jumpTo(old.pixels);
    }
    this.#controller.attach(p);
    this.#position = p;
  }

  override didUpdateWidget(old: Scrollable): void {
    if (old.controller !== this.widget.controller || old.axis !== this.widget.axis) {
      const oldController = old.controller ?? this.#fallbackController;
      if (this.#position !== null) {
        oldController?.detach(this.#position);
        const prev = this.#position;
        this.#createPosition();
        prev.dispose();
      }
    }
  }

  override dispose(): void {
    if (this.#position !== null) {
      this.#controller.detach(this.#position);
      this.#position.dispose();
    }
    this.#fallbackController?.dispose();
  }

  #handleSignal = (event: PointerSignalEvent): void => {
    if (!(event instanceof PointerScrollEvent)) return;
    const p = this.position;
    const delta = this.widget.axis === "vertical" ? event.scrollDelta.dy : event.scrollDelta.dx;
    const fallback = this.widget.axis === "vertical" ? 0 : event.scrollDelta.dy;
    const d = delta !== 0 ? delta : fallback;
    const target = Math.min(Math.max(p.pixels + d, p.minScrollExtent), p.maxScrollExtent);
    if (target !== p.pixels) {
      GestureBinding.instance.pointerSignalResolver.register(event, (e) => {
        if (e instanceof PointerScrollEvent) p.pointerScroll(d);
      });
    }
  };

  #onDragStart = (): void => this.position.dragStart();
  #onDragUpdate = (d: DragUpdateDetails): void => this.position.dragUpdate(d.primaryDelta ?? 0);
  #onDragEnd = (d: DragEndDetails): void => this.position.dragEnd(d.primaryVelocity ?? 0);
  #onDragCancel = (): void => this.position.dragCancel();

  build(context: BuildContext): Widget {
    const vertical = this.widget.axis === "vertical";
    const gestures = new Map<Type<GestureRecognizer>, GestureRecognizerFactory<GestureRecognizer>>();
    const devices = this.widget.dragDevices;
    if (vertical) {
      gestures.set(VerticalDragGestureRecognizer, gestureFactory(() => new VerticalDragGestureRecognizer(), (r: VerticalDragGestureRecognizer) => {
        r.supportedDevices = devices;
        r.onStart = this.#onDragStart;
        r.onUpdate = this.#onDragUpdate;
        r.onEnd = this.#onDragEnd;
        r.onCancel = this.#onDragCancel;
      }));
    } else {
      gestures.set(HorizontalDragGestureRecognizer, gestureFactory(() => new HorizontalDragGestureRecognizer(), (r: HorizontalDragGestureRecognizer) => {
        r.supportedDevices = devices;
        r.onStart = this.#onDragStart;
        r.onUpdate = (d) => this.position.dragUpdate(d.primaryDelta ?? 0);
        r.onEnd = this.#onDragEnd;
        r.onCancel = this.#onDragCancel;
      }));
    }
    return new ScrollableScope(this, this.position, new Listener({
      onPointerSignal: this.#handleSignal,
      child: new RawGestureDetector({
        gestures,
        behavior: "opaque",
        child: this.widget.viewportBuilder(context, this.position),
      }),
    }));
  }
}
