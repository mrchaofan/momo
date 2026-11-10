import { Paint, Path, type Canvas } from "momo:ui";
import type { Key } from "../foundation/key";
import { BorderRadius, drawRRect } from "../painting/borders";
import type { Color } from "../painting/color";
import { Rect, Size } from "../painting/geometry";
import { CustomPainter } from "../rendering/proxy_box";
import { ActivateIntent, Actions, CallbackAction } from "../widgets/actions";
import { CustomPaint, MouseRegion, Padding, EdgeInsets } from "./internal";
import { Focus } from "../widgets/focus_scope";
import { GestureDetector } from "../widgets/gesture_detector";
import { State, StatefulWidget, type BuildContext, type Widget } from "../widgets/framework";
import { Theme } from "./theme";

const paint = new Paint();

class CheckboxPainter extends CustomPainter {
  constructor(readonly value: boolean, readonly fill: Color, readonly border: Color, readonly check: Color, readonly halo: Color | null) {
    super();
  }
  paint(canvas: Canvas, size: Size): void {
    const s = 18;
    const x = (size.width - s) / 2;
    const y = (size.height - s) / 2;
    paint.shader = null;
    paint.maskBlurSigma = 0;
    if (this.halo !== null) {
      paint.style = "fill";
      paint.color = this.halo.value;
      canvas.drawCircle(size.width / 2, size.height / 2, Math.min(size.width, size.height) / 2, paint);
    }
    const r = Rect.fromLTWH(x, y, s, s);
    if (this.value) {
      paint.style = "fill";
      paint.color = this.fill.value;
      drawRRect(canvas, r, BorderRadius.circular(2), paint);
      paint.style = "stroke";
      paint.strokeWidth = 2;
      paint.strokeCap = "round";
      paint.strokeJoin = "round";
      paint.color = this.check.value;
      const p = new Path().moveTo(x + 4, y + 9.5).lineTo(x + 7.5, y + 13).lineTo(x + 14, y + 5.5);
      canvas.drawPath(p, paint);
    } else {
      paint.style = "stroke";
      paint.strokeWidth = 2;
      paint.color = this.border.value;
      canvas.drawRRect(x + 1, y + 1, s - 2, s - 2, 2, paint);
    }
    paint.style = "fill";
  }
  override shouldRepaint(old: CheckboxPainter): boolean {
    return old.value !== this.value || !old.fill.equals(this.fill) || !old.border.equals(this.border) || old.halo?.value !== this.halo?.value;
  }
}

/** 复选框。onChanged 为 null 时禁用。 */
export class Checkbox extends StatefulWidget {
  readonly value: boolean;
  readonly onChanged: ((value: boolean) => void) | null;
  readonly activeColor: Color | null;
  constructor(o: { key?: Key | null; value: boolean; onChanged: ((value: boolean) => void) | null; activeColor?: Color | null }) {
    super(o.key);
    this.value = o.value;
    this.onChanged = o.onChanged;
    this.activeColor = o.activeColor ?? null;
  }
  createState(): State {
    return new CheckboxState();
  }
}

class CheckboxState extends State<Checkbox> {
  #hovered = false;
  #focused = false;
  #toggle = (): void => {
    this.widget.onChanged?.(!this.widget.value);
  };
  build(context: BuildContext): Widget {
    const theme = Theme.of(context);
    const cs = theme.colorScheme;
    const enabled = this.widget.onChanged !== null;
    const active = this.widget.activeColor ?? cs.primary;
    const fill = enabled ? active : cs.onSurface.withOpacity(0.38);
    const border = enabled ? cs.onSurface.withOpacity(0.7) : cs.onSurface.withOpacity(0.38);
    const halo = enabled && (this.#hovered || this.#focused) ? (this.widget.value ? active : cs.onSurface).withOpacity(this.#focused ? 0.12 : 0.08) : null;
    return new Actions({
      actions: new Map([[ActivateIntent, new CallbackAction<ActivateIntent>({ onInvoke: () => this.#toggle(), isEnabled: () => enabled })]]),
      child: new Focus({
        canRequestFocus: enabled,
        onFocusChange: (f) => this.setState(() => (this.#focused = f)),
        child: new MouseRegion({
          cursor: enabled ? "click" : "basic",
          onEnter: () => this.setState(() => (this.#hovered = true)),
          onExit: () => this.setState(() => (this.#hovered = false)),
          child: new GestureDetector({
            behavior: "opaque",
            onTap: enabled ? this.#toggle : null,
            child: new Padding({
              padding: EdgeInsets.all(3),
              child: new CustomPaint({ size: new Size(34, 34), painter: new CheckboxPainter(this.widget.value, fill, border, cs.onPrimary, halo) }),
            }),
          }),
        }),
      }),
    });
  }
}
