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

/** 系统窗口控制按钮不占用内容区时(未隐藏标题栏/平台不支持), 标签栏左侧的留白(逻辑像素)。 */
export const kStripLeadingPadding = 8;
/**
 * 标签栏 = 顶部缩放边 + 标签行(逻辑像素)。顶部这条细边不放标签: 系统的顶边缩放区落在这里,
 * 标签整体在它下面, 指针移进标签就不会再出现缩放光标、按下也不会变成缩放。
 */
export const kTabStripTopInset = 8;
export const kTabHeight = 38;
export const kTabStripHeight = kTabStripTopInset + kTabHeight;
/**
 * 声明给宿主的标题栏高度: 系统窗口控制按钮在这个高度内垂直居中, 中线与标签行的中线对齐
 * (标签行在顶部缩放边之下)。
 */
export const kTabStripTitleBarHeight = 2 * (kTabStripTopInset + kTabHeight / 2);
/** 单独窗口的顶栏高度(窗口控制按钮 + 标题 + 图标按钮)。 */
export const kTitleBarHeight = 38;
export const kToolbarHeight = 46;
/** 插入/删除标签的动画时长(毫秒)。 */
export const kTabAnimationMs = 220;
