import { ChangeNotifier } from "../foundation/change_notifier";

/** 视口滚动偏移(像素)。滚动位置(ScrollPosition)实现它;视口布局后回报视口与内容尺寸。 */
export abstract class ViewportOffset extends ChangeNotifier {
  /** 当前滚动像素(内容相对视口的偏移)。 */
  abstract get pixels(): number;
  /** 视口在主轴上的尺寸;返回 false 表示 pixels 因此被修正, 需要重新布局。 */
  abstract applyViewportDimension(viewportDimension: number): boolean;
  /** 内容的滚动范围;返回 false 表示 pixels 被修正, 需要重新布局。 */
  abstract applyContentDimensions(minScrollExtent: number, maxScrollExtent: number): boolean;

  /** 固定偏移(不可滚动)。 */
  static fixed(value: number): ViewportOffset {
    return new FixedViewportOffset(value);
  }
  /** 0 偏移。 */
  static zero(): ViewportOffset {
    return new FixedViewportOffset(0);
  }
}

class FixedViewportOffset extends ViewportOffset {
  constructor(readonly value: number) {
    super();
  }
  get pixels(): number {
    return this.value;
  }
  applyViewportDimension(): boolean {
    return true;
  }
  applyContentDimensions(): boolean {
    return true;
  }
}
