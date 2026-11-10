import type { Key } from "../foundation/key";
import {
  HorizontalDragGestureRecognizer, PanGestureRecognizer, VerticalDragGestureRecognizer,
  type DragDownDetails, type DragEndDetails, type DragStartDetails, type DragUpdateDetails,
} from "../gestures/drag";
import type { PointerDeviceKind, PointerDownEvent } from "../gestures/events";
import { LongPressGestureRecognizer, type LongPressDetails } from "../gestures/long_press";
import { DoubleTapGestureRecognizer } from "../gestures/multitap";
import type { GestureRecognizer } from "../gestures/recognizer";
import { TapGestureRecognizer, type TapDownDetails, type TapUpDetails } from "../gestures/tap";
import type { HitTestBehavior } from "../rendering/box";
import { Listener } from "./basic";
import { State, StatefulWidget, StatelessWidget, type BuildContext, type Type, type Widget } from "./framework";

/** 识别器工厂:创建 + (每次 widget 更新时)配置。 */
export interface GestureRecognizerFactory<T extends GestureRecognizer = GestureRecognizer> {
  create(): T;
  initialize(recognizer: T): void;
}

/** 创建识别器工厂。 */
export function gestureFactory<T extends GestureRecognizer>(create: () => T, initialize: (r: T) => void): GestureRecognizerFactory<T> {
  return { create, initialize };
}

/** 识别器类型 → 工厂。 */
export type GestureFactories = ReadonlyMap<Type<GestureRecognizer>, GestureRecognizerFactory<GestureRecognizer>>;

/** 直接使用识别器的手势检测。 */
export class RawGestureDetector extends StatefulWidget {
  readonly gestures: GestureFactories;
  readonly behavior: HitTestBehavior | null;
  readonly child: Widget | null;
  constructor(o: { key?: Key | null; gestures: GestureFactories; behavior?: HitTestBehavior | null; child?: Widget | null }) {
    super(o.key);
    this.gestures = o.gestures;
    this.behavior = o.behavior ?? null;
    this.child = o.child ?? null;
  }
  createState(): State {
    return new RawGestureDetectorState();
  }
}

export class RawGestureDetectorState extends State<RawGestureDetector> {
  #recognizers = new Map<Type<GestureRecognizer>, GestureRecognizer>();

  override initState(): void {
    this.#sync(this.widget.gestures);
  }
  override didUpdateWidget(_old: RawGestureDetector): void {
    this.#sync(this.widget.gestures);
  }
  override dispose(): void {
    for (const r of this.#recognizers.values()) r.dispose();
    this.#recognizers.clear();
  }

  /** 当前识别器(测试用)。 */
  get recognizers(): ReadonlyMap<Type<GestureRecognizer>, GestureRecognizer> {
    return this.#recognizers;
  }

  #sync(gestures: GestureFactories): void {
    const old = this.#recognizers;
    const next = new Map<Type<GestureRecognizer>, GestureRecognizer>();
    for (const [type, factory] of gestures) {
      const f = factory;
      const r = old.get(type) ?? f.create();
      f.initialize(r);
      next.set(type, r);
    }
    for (const [type, r] of old) if (!next.has(type)) r.dispose();
    this.#recognizers = next;
  }

  #handlePointerDown = (event: PointerDownEvent): void => {
    for (const r of this.#recognizers.values()) r.addPointer(event);
  };

  build(_context: BuildContext): Widget {
    return new Listener({
      onPointerDown: this.#handlePointerDown,
      behavior: this.widget.behavior ?? (this.widget.child !== null ? "deferToChild" : "translucent"),
      child: this.widget.child,
    });
  }
}

/** GestureDetector 回调参数。 */
export interface GestureDetectorOptions {
  key?: Key | null;
  child?: Widget | null;
  behavior?: HitTestBehavior | null;
  supportedDevices?: ReadonlySet<PointerDeviceKind> | null;
  onTapDown?: ((d: TapDownDetails) => void) | null;
  onTapUp?: ((d: TapUpDetails) => void) | null;
  onTap?: (() => void) | null;
  onTapCancel?: (() => void) | null;
  onSecondaryTap?: (() => void) | null;
  onSecondaryTapDown?: ((d: TapDownDetails) => void) | null;
  onSecondaryTapUp?: ((d: TapUpDetails) => void) | null;
  onDoubleTap?: (() => void) | null;
  onLongPress?: (() => void) | null;
  onLongPressStart?: ((d: LongPressDetails) => void) | null;
  onLongPressEnd?: ((d: LongPressDetails) => void) | null;
  onVerticalDragDown?: ((d: DragDownDetails) => void) | null;
  onVerticalDragStart?: ((d: DragStartDetails) => void) | null;
  onVerticalDragUpdate?: ((d: DragUpdateDetails) => void) | null;
  onVerticalDragEnd?: ((d: DragEndDetails) => void) | null;
  onVerticalDragCancel?: (() => void) | null;
  onHorizontalDragDown?: ((d: DragDownDetails) => void) | null;
  onHorizontalDragStart?: ((d: DragStartDetails) => void) | null;
  onHorizontalDragUpdate?: ((d: DragUpdateDetails) => void) | null;
  onHorizontalDragEnd?: ((d: DragEndDetails) => void) | null;
  onHorizontalDragCancel?: (() => void) | null;
  onPanDown?: ((d: DragDownDetails) => void) | null;
  onPanStart?: ((d: DragStartDetails) => void) | null;
  onPanUpdate?: ((d: DragUpdateDetails) => void) | null;
  onPanEnd?: ((d: DragEndDetails) => void) | null;
  onPanCancel?: (() => void) | null;
}

/** 常用手势检测(按提供的回调创建 tap/double-tap/long-press/drag/pan 识别器, 由竞技场消歧)。 */
export class GestureDetector extends StatelessWidget {
  readonly o: GestureDetectorOptions;
  constructor(o: GestureDetectorOptions) {
    super(o.key);
    this.o = o;
  }

  build(_context: BuildContext): Widget {
    const o = this.o;
    const devices = o.supportedDevices ?? null;
    const g = new Map<Type<GestureRecognizer>, GestureRecognizerFactory<GestureRecognizer>>();
    if (o.onTapDown || o.onTapUp || o.onTap || o.onTapCancel || o.onSecondaryTap || o.onSecondaryTapDown || o.onSecondaryTapUp) {
      g.set(TapGestureRecognizer, gestureFactory(() => new TapGestureRecognizer({ debugOwner: this }), (r: TapGestureRecognizer) => {
        r.supportedDevices = devices;
        r.onTapDown = o.onTapDown ?? null;
        r.onTapUp = o.onTapUp ?? null;
        r.onTap = o.onTap ?? null;
        r.onTapCancel = o.onTapCancel ?? null;
        r.onSecondaryTap = o.onSecondaryTap ?? null;
        r.onSecondaryTapDown = o.onSecondaryTapDown ?? null;
        r.onSecondaryTapUp = o.onSecondaryTapUp ?? null;
      }));
    }
    if (o.onDoubleTap) {
      g.set(DoubleTapGestureRecognizer, gestureFactory(() => new DoubleTapGestureRecognizer({ debugOwner: this }), (r: DoubleTapGestureRecognizer) => {
        r.supportedDevices = devices;
        r.onDoubleTap = o.onDoubleTap ?? null;
      }));
    }
    if (o.onLongPress || o.onLongPressStart || o.onLongPressEnd) {
      g.set(LongPressGestureRecognizer, gestureFactory(() => new LongPressGestureRecognizer({ debugOwner: this }), (r: LongPressGestureRecognizer) => {
        r.supportedDevices = devices;
        r.onLongPress = o.onLongPress ?? null;
        r.onLongPressStart = o.onLongPressStart ?? null;
        r.onLongPressEnd = o.onLongPressEnd ?? null;
      }));
    }
    if (o.onVerticalDragDown || o.onVerticalDragStart || o.onVerticalDragUpdate || o.onVerticalDragEnd || o.onVerticalDragCancel) {
      g.set(VerticalDragGestureRecognizer, gestureFactory(() => new VerticalDragGestureRecognizer({ debugOwner: this }), (r: VerticalDragGestureRecognizer) => {
        r.supportedDevices = devices;
        r.onDown = o.onVerticalDragDown ?? null;
        r.onStart = o.onVerticalDragStart ?? null;
        r.onUpdate = o.onVerticalDragUpdate ?? null;
        r.onEnd = o.onVerticalDragEnd ?? null;
        r.onCancel = o.onVerticalDragCancel ?? null;
      }));
    }
    if (o.onHorizontalDragDown || o.onHorizontalDragStart || o.onHorizontalDragUpdate || o.onHorizontalDragEnd || o.onHorizontalDragCancel) {
      g.set(HorizontalDragGestureRecognizer, gestureFactory(() => new HorizontalDragGestureRecognizer({ debugOwner: this }), (r: HorizontalDragGestureRecognizer) => {
        r.supportedDevices = devices;
        r.onDown = o.onHorizontalDragDown ?? null;
        r.onStart = o.onHorizontalDragStart ?? null;
        r.onUpdate = o.onHorizontalDragUpdate ?? null;
        r.onEnd = o.onHorizontalDragEnd ?? null;
        r.onCancel = o.onHorizontalDragCancel ?? null;
      }));
    }
    if (o.onPanDown || o.onPanStart || o.onPanUpdate || o.onPanEnd || o.onPanCancel) {
      g.set(PanGestureRecognizer, gestureFactory(() => new PanGestureRecognizer({ debugOwner: this }), (r: PanGestureRecognizer) => {
        r.supportedDevices = devices;
        r.onDown = o.onPanDown ?? null;
        r.onStart = o.onPanStart ?? null;
        r.onUpdate = o.onPanUpdate ?? null;
        r.onEnd = o.onPanEnd ?? null;
        r.onCancel = o.onPanCancel ?? null;
      }));
    }
    return new RawGestureDetector({ gestures: g, behavior: o.behavior ?? null, child: o.child ?? null });
  }
}
