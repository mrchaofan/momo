// 菜单快捷键(对标 Electron Accelerator): "CmdOrCtrl+Shift+N"、"Alt+F4"、"F11"。
// 这里只把字符串解析成规范形式;快捷键由宿主识别(macOS: 菜单的 key equivalent;Windows: 宿主在按键到达
// 应用内容之前匹配), 命中后与点击菜单项相同。
import { isApplePlatform } from "../foundation/platform";

/** 修饰键位(与引擎 KeyData.modifiers 相同)。 */
export const AcceleratorModifier = { shift: 1, control: 2, alt: 4, meta: 8 } as const;

/** 解析后的快捷键。key: 单个小写字符("n"、"1"、"+"), 或具名键("F5"、"Enter"、"Up"…)。 */
export interface Accelerator {
  readonly key: string;
  readonly modifiers: number;
}

const kModifiers: Readonly<Record<string, number | "primary">> = {
  shift: AcceleratorModifier.shift,
  control: AcceleratorModifier.control,
  ctrl: AcceleratorModifier.control,
  alt: AcceleratorModifier.alt,
  option: AcceleratorModifier.alt,
  command: AcceleratorModifier.meta,
  cmd: AcceleratorModifier.meta,
  super: AcceleratorModifier.meta,
  meta: AcceleratorModifier.meta,
  commandorcontrol: "primary",
  cmdorctrl: "primary",
};

const kNamedKeys: Readonly<Record<string, string>> = {
  enter: "Enter", return: "Enter", tab: "Tab", space: "Space", backspace: "Backspace", delete: "Delete",
  insert: "Insert", escape: "Escape", esc: "Escape", up: "Up", down: "Down", left: "Left", right: "Right",
  home: "Home", end: "End", pageup: "PageUp", pagedown: "PageDown", plus: "+",
};

/**
 * 解析快捷键字符串: 以 "+" 连接的若干修饰键加一个按键, 不区分大小写。修饰键: Command(Cmd)、Control(Ctrl)、
 * CommandOrControl(CmdOrCtrl: macOS 为 Command, 其他平台为 Control)、Alt(Option)、Shift、Super(Meta)。
 * 按键: 单个字符、F1–F24、Plus、Space、Tab、Backspace、Delete、Insert、Enter(Return)、Escape(Esc)、
 * Up/Down/Left/Right、Home、End、PageUp、PageDown。格式不对时抛 TypeError。
 */
export function parseAccelerator(text: string): Accelerator {
  // "Ctrl++" 的最后一段是 "+" 本身。
  const parts = text.endsWith("++") ? [...text.slice(0, -2).split("+"), "+"] : text.split("+");
  let modifiers = 0;
  let key: string | null = null;
  for (const raw of parts) {
    const part = raw.trim();
    const lower = part.toLowerCase();
    const modifier = kModifiers[lower];
    if (modifier !== undefined) {
      modifiers |= modifier === "primary" ? (isApplePlatform() ? AcceleratorModifier.meta : AcceleratorModifier.control)
        : modifier;
      continue;
    }
    if (key !== null || part === "") throw new TypeError(`Invalid accelerator "${text}"`);
    if (kNamedKeys[lower] !== undefined) key = kNamedKeys[lower];
    else if (/^f([1-9]|1[0-9]|2[0-4])$/.test(lower)) key = lower.toUpperCase();
    else if ([...part].length === 1) key = lower;
    else throw new TypeError(`Invalid accelerator "${text}": unknown key "${part}"`);
  }
  if (key === null) throw new TypeError(`Invalid accelerator "${text}": no key`);
  return { key, modifiers };
}
