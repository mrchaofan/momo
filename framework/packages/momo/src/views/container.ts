import type { Key } from "../foundation/key";
import type { Alignment } from "../painting/alignment";
import type { Color } from "../painting/color";
import { BoxDecoration, type Decoration } from "../painting/decoration";
import type { EdgeInsets } from "../painting/edge_insets";
import type { Clip } from "../painting/geometry";
import type { Matrix4 } from "../painting/matrix4";
import { BoxConstraints } from "../rendering/box_constraints";
import { Align, ClipRRect, ClipRect, ColoredBox, ConstrainedBox, DecoratedBox, LimitedBox, Padding, Transform } from "./basic";
import { StatelessView, type BuildContext, type View } from "./framework";

/** Container 参数。 */
export interface ContainerOptions {
  key?: Key | null;
  alignment?: Alignment | null;
  padding?: EdgeInsets | null;
  color?: Color | null;
  decoration?: Decoration | null;
  foregroundDecoration?: Decoration | null;
  width?: number | null;
  height?: number | null;
  constraints?: BoxConstraints | null;
  margin?: EdgeInsets | null;
  transform?: Matrix4 | null;
  clipBehavior?: Clip;
  child?: View | null;
}

/** 组合了对齐/内边距/装饰/约束/外边距/变换的便利容器(与 Flutter Container 相同的嵌套顺序)。 */
export class Container extends StatelessView {
  readonly o: ContainerOptions;
  constructor(o: ContainerOptions = {}) {
    super(o.key);
    if (o.color != null && o.decoration != null) throw new Error("Container: cannot provide both a color and a decoration");
    this.o = o;
  }

  build(_context: BuildContext): View {
    const o = this.o;
    let constraints = o.constraints ?? null;
    if (o.width != null || o.height != null) {
      constraints = constraints !== null
        ? constraints.tighten({ width: o.width ?? null, height: o.height ?? null })
        : BoxConstraints.tightFor({ width: o.width ?? null, height: o.height ?? null });
    }
    let current: View | null = o.child ?? null;
    if (current === null && (constraints === null || !constraints.isTight)) {
      current = new LimitedBox({ maxWidth: 0, maxHeight: 0, child: new ConstrainedBox({ constraints: BoxConstraints.expand() }) });
    } else if (o.alignment != null && current !== null) {
      current = new Align({ alignment: o.alignment, child: current });
    }
    const decoPadding = o.decoration?.padding ?? null;
    let padding = o.padding ?? null;
    if (decoPadding !== null && (decoPadding.horizontal > 0 || decoPadding.vertical > 0)) padding = padding !== null ? padding.add(decoPadding) : decoPadding;
    if (padding !== null) current = new Padding({ padding, child: current });
    if (o.color != null) current = new ColoredBox({ color: o.color, child: current });
    const clip = o.clipBehavior ?? "none";
    if (clip !== "none" && o.decoration != null) {
      const br = o.decoration instanceof BoxDecoration ? o.decoration.borderRadius : null;
      current = br !== null ? new ClipRRect({ borderRadius: br, clipBehavior: clip, child: current }) : new ClipRect({ clipBehavior: clip, child: current });
    }
    if (o.decoration != null) current = new DecoratedBox({ decoration: o.decoration, child: current });
    if (o.foregroundDecoration != null) current = new DecoratedBox({ decoration: o.foregroundDecoration, position: "foreground", child: current });
    if (constraints !== null) current = new ConstrainedBox({ constraints, child: current });
    if (o.margin != null) current = new Padding({ padding: o.margin, child: current });
    if (o.transform != null) current = new Transform({ transform: o.transform, child: current });
    return current!;
  }
}
