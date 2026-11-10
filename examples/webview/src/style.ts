// 黑白简约风格的配色与文字样式。
import { Color, FontWeight, TextStyle } from "@momo/framework";

export const Palette = {
  black: new Color(0xff111111),
  ink: new Color(0xff2b2b2b),
  muted: new Color(0xff8a8a8a),
  faint: new Color(0xffc4c4c4),
  line: new Color(0xffe6e6e6),
  hover: new Color(0xfff2f2f2),
  field: new Color(0xfff5f5f5),
  white: new Color(0xffffffff),
};

export const TextStyles = {
  tab: new TextStyle({ fontSize: 12.5, color: Palette.muted }),
  tabActive: new TextStyle({ fontSize: 12.5, color: Palette.black, fontWeight: FontWeight.w600 }),
  icon: new TextStyle({ fontSize: 17, color: Palette.ink }),
  iconDisabled: new TextStyle({ fontSize: 17, color: Palette.faint }),
  close: new TextStyle({ fontSize: 13, color: Palette.muted }),
  address: new TextStyle({ fontSize: 13, color: Palette.black }),
  status: new TextStyle({ fontSize: 11, color: Palette.muted }),
  menu: new TextStyle({ fontSize: 13, color: Palette.black }),
  menuHint: new TextStyle({ fontSize: 13, color: Palette.muted }),
  menuDisabled: new TextStyle({ fontSize: 13, color: Palette.faint }),
};

/** 无边框窗口左上角留给红绿灯的宽度(逻辑像素)。 */
export const kTrafficLightsWidth = 78;
/** 标签栏、工具栏高度(逻辑像素)。 */
export const kTabStripHeight = 38;
export const kToolbarHeight = 46;
/** 插入/删除标签的动画时长(毫秒)。 */
export const kTabAnimationMs = 220;
