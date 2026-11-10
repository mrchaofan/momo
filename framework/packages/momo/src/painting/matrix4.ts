import { Offset, Rect } from "./geometry";

/**
 * 4x4 矩阵, 列主序(与 momo:ui Matrix4 / Flutter 一致):storage[col * 4 + row]。
 * 方法多为原地修改(返回 this), 以减少分配。
 */
export class Matrix4 {
  readonly storage: Float64Array;

  constructor(storage?: ArrayLike<number>) {
    this.storage = new Float64Array(16);
    if (storage) this.storage.set(storage);
    else this.setIdentity();
  }

  static identity(): Matrix4 {
    return new Matrix4();
  }
  static translationValues(x: number, y: number, z = 0): Matrix4 {
    return new Matrix4().translate(x, y, z);
  }
  static diagonal3Values(x: number, y: number, z: number): Matrix4 {
    return new Matrix4().scale(x, y, z);
  }
  static rotationZ(radians: number): Matrix4 {
    return new Matrix4().rotateZ(radians);
  }

  clone(): Matrix4 {
    return new Matrix4(this.storage);
  }

  setFrom(other: Matrix4): this {
    this.storage.set(other.storage);
    return this;
  }

  setIdentity(): this {
    const s = this.storage;
    s.fill(0);
    s[0] = s[5] = s[10] = s[15] = 1;
    return this;
  }

  get isIdentity(): boolean {
    const s = this.storage;
    for (let i = 0; i < 16; i++) if (s[i] !== (i % 5 === 0 ? 1 : 0)) return false;
    return true;
  }

  /** 若矩阵只是平移, 返回平移量, 否则 null。 */
  getAsTranslation(): Offset | null {
    const s = this.storage;
    if (s[0] === 1 && s[1] === 0 && s[2] === 0 && s[3] === 0 && s[4] === 0 && s[5] === 1 && s[6] === 0 && s[7] === 0 &&
        s[8] === 0 && s[9] === 0 && s[10] === 1 && s[11] === 0 && s[14] === 0 && s[15] === 1) {
      return new Offset(s[12]!, s[13]!);
    }
    return null;
  }

  /** this = this × T(x, y, z)。 */
  translate(x: number, y: number, z = 0): this {
    const s = this.storage;
    for (let r = 0; r < 4; r++) {
      s[12 + r] = s[r]! * x + s[4 + r]! * y + s[8 + r]! * z + s[12 + r]!;
    }
    return this;
  }

  /** this = this × S(x, y, z)。 */
  scale(x: number, y: number = x, z = 1): this {
    const s = this.storage;
    for (let r = 0; r < 4; r++) {
      s[r] = s[r]! * x;
      s[4 + r] = s[4 + r]! * y;
      s[8 + r] = s[8 + r]! * z;
    }
    return this;
  }

  /** this = this × Rz(radians)。 */
  rotateZ(radians: number): this {
    const c = Math.cos(radians);
    const sn = Math.sin(radians);
    const s = this.storage;
    for (let r = 0; r < 4; r++) {
      const a = s[r]!;
      const b = s[4 + r]!;
      s[r] = a * c + b * sn;
      s[4 + r] = b * c - a * sn;
    }
    return this;
  }

  /** this = this × other。 */
  multiply(other: Matrix4): this {
    const a = this.storage;
    const b = other.storage;
    const r = tmp;
    for (let col = 0; col < 4; col++) {
      for (let row = 0; row < 4; row++) {
        let sum = 0;
        for (let k = 0; k < 4; k++) sum += a[k * 4 + row]! * b[col * 4 + k]!;
        r[col * 4 + row] = sum;
      }
    }
    a.set(r);
    return this;
  }

  /** this = other × this。 */
  leftMultiply(other: Matrix4): this {
    const a = other.storage;
    const b = this.storage;
    const r = tmp;
    for (let col = 0; col < 4; col++) {
      for (let row = 0; row < 4; row++) {
        let sum = 0;
        for (let k = 0; k < 4; k++) sum += a[k * 4 + row]! * b[col * 4 + k]!;
        r[col * 4 + row] = sum;
      }
    }
    b.set(r);
    return this;
  }

  /** 求逆(原地);奇异矩阵返回 false 且不修改。 */
  invert(): boolean {
    const m = this.storage;
    const inv = tmp;
    inv[0] = m[5]! * m[10]! * m[15]! - m[5]! * m[11]! * m[14]! - m[9]! * m[6]! * m[15]! + m[9]! * m[7]! * m[14]! + m[13]! * m[6]! * m[11]! - m[13]! * m[7]! * m[10]!;
    inv[4] = -m[4]! * m[10]! * m[15]! + m[4]! * m[11]! * m[14]! + m[8]! * m[6]! * m[15]! - m[8]! * m[7]! * m[14]! - m[12]! * m[6]! * m[11]! + m[12]! * m[7]! * m[10]!;
    inv[8] = m[4]! * m[9]! * m[15]! - m[4]! * m[11]! * m[13]! - m[8]! * m[5]! * m[15]! + m[8]! * m[7]! * m[13]! + m[12]! * m[5]! * m[11]! - m[12]! * m[7]! * m[9]!;
    inv[12] = -m[4]! * m[9]! * m[14]! + m[4]! * m[10]! * m[13]! + m[8]! * m[5]! * m[14]! - m[8]! * m[6]! * m[13]! - m[12]! * m[5]! * m[10]! + m[12]! * m[6]! * m[9]!;
    inv[1] = -m[1]! * m[10]! * m[15]! + m[1]! * m[11]! * m[14]! + m[9]! * m[2]! * m[15]! - m[9]! * m[3]! * m[14]! - m[13]! * m[2]! * m[11]! + m[13]! * m[3]! * m[10]!;
    inv[5] = m[0]! * m[10]! * m[15]! - m[0]! * m[11]! * m[14]! - m[8]! * m[2]! * m[15]! + m[8]! * m[3]! * m[14]! + m[12]! * m[2]! * m[11]! - m[12]! * m[3]! * m[10]!;
    inv[9] = -m[0]! * m[9]! * m[15]! + m[0]! * m[11]! * m[13]! + m[8]! * m[1]! * m[15]! - m[8]! * m[3]! * m[13]! - m[12]! * m[1]! * m[11]! + m[12]! * m[3]! * m[9]!;
    inv[13] = m[0]! * m[9]! * m[14]! - m[0]! * m[10]! * m[13]! - m[8]! * m[1]! * m[14]! + m[8]! * m[2]! * m[13]! + m[12]! * m[1]! * m[10]! - m[12]! * m[2]! * m[9]!;
    inv[2] = m[1]! * m[6]! * m[15]! - m[1]! * m[7]! * m[14]! - m[5]! * m[2]! * m[15]! + m[5]! * m[3]! * m[14]! + m[13]! * m[2]! * m[7]! - m[13]! * m[3]! * m[6]!;
    inv[6] = -m[0]! * m[6]! * m[15]! + m[0]! * m[7]! * m[14]! + m[4]! * m[2]! * m[15]! - m[4]! * m[3]! * m[14]! - m[12]! * m[2]! * m[7]! + m[12]! * m[3]! * m[6]!;
    inv[10] = m[0]! * m[5]! * m[15]! - m[0]! * m[7]! * m[13]! - m[4]! * m[1]! * m[15]! + m[4]! * m[3]! * m[13]! + m[12]! * m[1]! * m[7]! - m[12]! * m[3]! * m[5]!;
    inv[14] = -m[0]! * m[5]! * m[14]! + m[0]! * m[6]! * m[13]! + m[4]! * m[1]! * m[14]! - m[4]! * m[2]! * m[13]! - m[12]! * m[1]! * m[6]! + m[12]! * m[2]! * m[5]!;
    inv[3] = -m[1]! * m[6]! * m[11]! + m[1]! * m[7]! * m[10]! + m[5]! * m[2]! * m[11]! - m[5]! * m[3]! * m[10]! - m[9]! * m[2]! * m[7]! + m[9]! * m[3]! * m[6]!;
    inv[7] = m[0]! * m[6]! * m[11]! - m[0]! * m[7]! * m[10]! - m[4]! * m[2]! * m[11]! + m[4]! * m[3]! * m[10]! + m[8]! * m[2]! * m[7]! - m[8]! * m[3]! * m[6]!;
    inv[11] = -m[0]! * m[5]! * m[11]! + m[0]! * m[7]! * m[9]! + m[4]! * m[1]! * m[11]! - m[4]! * m[3]! * m[9]! - m[8]! * m[1]! * m[7]! + m[8]! * m[3]! * m[5]!;
    inv[15] = m[0]! * m[5]! * m[10]! - m[0]! * m[6]! * m[9]! - m[4]! * m[1]! * m[10]! + m[4]! * m[2]! * m[9]! + m[8]! * m[1]! * m[6]! - m[8]! * m[2]! * m[5]!;
    const det = m[0]! * inv[0]! + m[1]! * inv[4]! + m[2]! * inv[8]! + m[3]! * inv[12]!;
    if (det === 0) return false;
    const invDet = 1 / det;
    for (let i = 0; i < 16; i++) m[i] = inv[i]! * invDet;
    return true;
  }

  /** 返回逆矩阵副本;奇异时 null。 */
  inverted(): Matrix4 | null {
    const m = this.clone();
    return m.invert() ? m : null;
  }

  /** 变换二维点(z = 0, 透视除法)。 */
  transformPoint(p: Offset): Offset {
    const s = this.storage;
    const x = p.dx;
    const y = p.dy;
    const rx = s[0]! * x + s[4]! * y + s[12]!;
    const ry = s[1]! * x + s[5]! * y + s[13]!;
    const w = s[3]! * x + s[7]! * y + s[15]!;
    return w === 1 ? new Offset(rx, ry) : new Offset(rx / w, ry / w);
  }

  /** 变换矩形, 返回包围盒。 */
  transformRect(r: Rect): Rect {
    const a = this.transformPoint(new Offset(r.left, r.top));
    const b = this.transformPoint(new Offset(r.right, r.top));
    const c = this.transformPoint(new Offset(r.left, r.bottom));
    const d = this.transformPoint(new Offset(r.right, r.bottom));
    return new Rect(Math.min(a.dx, b.dx, c.dx, d.dx), Math.min(a.dy, b.dy, c.dy, d.dy),
                    Math.max(a.dx, b.dx, c.dx, d.dx), Math.max(a.dy, b.dy, c.dy, d.dy));
  }

  equals(other: Matrix4 | null | undefined): boolean {
    if (!other) return false;
    for (let i = 0; i < 16; i++) if (this.storage[i] !== other.storage[i]) return false;
    return true;
  }
}

const tmp = new Float64Array(16);
