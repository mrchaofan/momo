import { Offset, Rect, Size, lerpDouble } from "./geometry";

/** 矩形内的对齐点:(-1,-1) 左上, (0,0) 中心, (1,1) 右下。 */
export class Alignment {
  static readonly topLeft = new Alignment(-1, -1);
  static readonly topCenter = new Alignment(0, -1);
  static readonly topRight = new Alignment(1, -1);
  static readonly centerLeft = new Alignment(-1, 0);
  static readonly center = new Alignment(0, 0);
  static readonly centerRight = new Alignment(1, 0);
  static readonly bottomLeft = new Alignment(-1, 1);
  static readonly bottomCenter = new Alignment(0, 1);
  static readonly bottomRight = new Alignment(1, 1);

  constructor(readonly x: number, readonly y: number) {}

  /** 在 size 大小的"剩余空间"里对应的偏移(用于子在父中的定位)。 */
  alongOffset(other: Offset): Offset {
    const cx = other.dx / 2;
    const cy = other.dy / 2;
    return new Offset(cx + this.x * cx, cy + this.y * cy);
  }
  /** size 内的对齐点。 */
  alongSize(other: Size): Offset {
    const cx = other.width / 2;
    const cy = other.height / 2;
    return new Offset(cx + this.x * cx, cy + this.y * cy);
  }
  withinRect(rect: Rect): Offset {
    const hw = rect.width / 2;
    const hh = rect.height / 2;
    return new Offset(rect.left + hw + this.x * hw, rect.top + hh + this.y * hh);
  }
  /** 把 size 大小的矩形按本对齐放进 rect。 */
  inscribe(size: Size, rect: Rect): Rect {
    const hw = (rect.width - size.width) / 2;
    const hh = (rect.height - size.height) / 2;
    return Rect.fromLTWH(rect.left + hw + this.x * hw, rect.top + hh + this.y * hh, size.width, size.height);
  }
  equals(o: Alignment | null | undefined): boolean {
    return o != null && o.x === this.x && o.y === this.y;
  }
  static lerp(a: Alignment, b: Alignment, t: number): Alignment {
    return new Alignment(lerpDouble(a.x, b.x, t), lerpDouble(a.y, b.y, t));
  }
  toString(): string {
    return `Alignment(${this.x}, ${this.y})`;
  }
}
