// 黑白线性图标(自绘, 16×16 设计网格, 按尺寸缩放)与图标按钮。
import { Paint, Path, type Canvas } from "momo:ui";
import {
  BorderRadius, BoxDecoration, Center, Container, CustomPaint, CustomPainter, GestureDetector, MouseRegion, Size, State,
  StatefulWidget, type BuildContext, type Widget,
} from "@momo/framework";
import { Palette } from "./style";

export type IconKind = "pin" | "restore";

class IconPainter extends CustomPainter {
  constructor(readonly kind: IconKind, readonly color: number) {
    super();
  }
  paint(canvas: Canvas, size: Size): void {
    const s = size.width / 16;
    const p = new Paint();
    p.color = this.color;
    p.style = "stroke";
    p.strokeWidth = 1.4 * s;
    p.strokeCap = "round";
    p.strokeJoin = "round";
    p.antiAlias = true;
    const path = new Path();
    const m = (x: number, y: number) => path.moveTo(x * s, y * s);
    const l = (x: number, y: number) => path.lineTo(x * s, y * s);
    if (this.kind === "pin") {
      // 图钉: 顶帽、钉身、底座、钉尖。
      m(5.5, 2.5); l(10.5, 2.5);
      m(6.5, 2.5); l(6, 8.5);
      m(9.5, 2.5); l(10, 8.5);
      m(3.5, 8.5); l(12.5, 8.5);
      m(8, 8.5); l(8, 14);
    } else {
      // 收回到标签页: 窗口轮廓 + 指向内部的箭头。
      path.addRRect(2.5 * s, 5 * s, 9 * s, 8.5 * s, 1.5 * s);
      m(14, 2); l(9, 7);
      m(9, 3.8); l(9, 7); l(12.2, 7);
    }
    canvas.drawPath(path, p);
  }
  override shouldRepaint(old: CustomPainter): boolean {
    return !(old instanceof IconPainter) || old.kind !== this.kind || old.color !== this.color;
  }
}

/** 图标按钮: 悬停浅灰底;active(如已置顶)为黑底白图标。 */
export class IconButton extends StatefulWidget {
  constructor(readonly o: { icon: IconKind; onTap: () => void; active?: boolean; size?: number }) {
    super();
  }
  createState(): State {
    return new IconButtonState();
  }
}

class IconButtonState extends State<IconButton> {
  #hover = false;
  build(_c: BuildContext): Widget {
    const { icon, onTap, active = false, size = 28 } = this.widget.o;
    const bg = active ? Palette.black : this.#hover ? Palette.hover : Palette.white;
    const fg = active ? Palette.white : Palette.ink;
    return new MouseRegion({
      cursor: "click",
      onEnter: () => this.setState(() => (this.#hover = true)),
      onExit: () => this.setState(() => (this.#hover = false)),
      child: new GestureDetector({
        behavior: "opaque",
        onTap,
        child: new Container({
          width: size,
          height: size,
          decoration: new BoxDecoration({ color: bg, borderRadius: BorderRadius.circular(6) }),
          child: new Center({ child: new CustomPaint({ size: new Size(16, 16), painter: new IconPainter(icon, fg.value) }) }),
        }),
      }),
    });
  }
}
