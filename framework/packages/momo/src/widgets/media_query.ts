import type { Key } from "../foundation/key";
import { Size } from "../painting/geometry";
import { InheritedWidget, type BuildContext, type Widget } from "./framework";

/** 窗口/显示信息。 */
export class MediaQueryData {
  /** 逻辑尺寸。 */
  readonly size: Size;
  readonly devicePixelRatio: number;
  readonly textScaleFactor: number;
  /** 窗口是否获得系统焦点。 */
  readonly windowFocused: boolean;
  constructor(o: { size?: Size; devicePixelRatio?: number; textScaleFactor?: number; windowFocused?: boolean } = {}) {
    this.size = o.size ?? Size.zero;
    this.devicePixelRatio = o.devicePixelRatio ?? 1;
    this.textScaleFactor = o.textScaleFactor ?? 1;
    this.windowFocused = o.windowFocused ?? true;
  }
  equals(o: MediaQueryData): boolean {
    return o.size.equals(this.size) && o.devicePixelRatio === this.devicePixelRatio && o.textScaleFactor === this.textScaleFactor && o.windowFocused === this.windowFocused;
  }
}

/** 向子树提供 MediaQueryData。 */
export class MediaQuery extends InheritedWidget {
  readonly data: MediaQueryData;
  constructor(o: { key?: Key | null; data: MediaQueryData; child: Widget }) {
    super(o);
    this.data = o.data;
  }
  static maybeOf(context: BuildContext): MediaQueryData | null {
    return context.dependOnInheritedWidgetOfExactType(MediaQuery)?.data ?? null;
  }
  static of(context: BuildContext): MediaQueryData {
    return MediaQuery.maybeOf(context) ?? new MediaQueryData();
  }
  updateShouldNotify(old: MediaQuery): boolean {
    return !old.data.equals(this.data);
  }
}
