import type { Key } from "../foundation/key";
import { Rect, Size } from "../painting/geometry";
import { InheritedWidget, type BuildContext, type Widget } from "./framework";

/** 窗口/显示信息。 */
export class MediaQueryData {
  /** 逻辑尺寸。 */
  readonly size: Size;
  readonly devicePixelRatio: number;
  readonly textScaleFactor: number;
  /** 窗口是否获得系统焦点。 */
  readonly windowFocused: boolean;
  /**
   * 系统窗口控制按钮(macOS 红绿灯、Windows 最小化/最大化/关闭)在本窗口内容区里占用的区域(逻辑像素);
   * 只有隐藏了系统标题栏(WindowChrome)时才占用内容区, 否则为 null。自绘标题栏据此避让, 不要写死平台尺寸
   * (同 Web 的 Window Controls Overlay)。
   */
  readonly windowControls: Rect | null;
  constructor(o: { size?: Size; devicePixelRatio?: number; textScaleFactor?: number; windowFocused?: boolean; windowControls?: Rect | null } = {}) {
    this.size = o.size ?? Size.zero;
    this.devicePixelRatio = o.devicePixelRatio ?? 1;
    this.textScaleFactor = o.textScaleFactor ?? 1;
    this.windowFocused = o.windowFocused ?? true;
    this.windowControls = o.windowControls ?? null;
  }
  equals(o: MediaQueryData): boolean {
    return o.size.equals(this.size) && o.devicePixelRatio === this.devicePixelRatio && o.textScaleFactor === this.textScaleFactor &&
      o.windowFocused === this.windowFocused &&
      (o.windowControls === null ? this.windowControls === null : o.windowControls.equals(this.windowControls));
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
