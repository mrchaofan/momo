// 原生菜单(对标 Electron Menu / MenuItem;协议见 embedder/common/menu_channel.h 的 momo/menu)。
//
//   const menu = Menu.buildFromTemplate([
//     { role: "appMenu" },
//     { label: "File", submenu: [
//       { label: "New Tab", accelerator: "CmdOrCtrl+T", click: () => openTab() },
//       { type: "separator" },
//       { role: "close" },
//     ] },
//     { role: "editMenu" },
//     { role: "windowMenu" },
//   ]);
//   Menu.setApplicationMenu(menu);                     // 应用菜单(macOS 菜单栏;Windows 每个窗口的菜单栏)
//   await menu.popup({ widget, x, y });                // 上下文菜单;菜单关闭后完成
//
// 分工: 菜单的内容与 click 回调在 JS;宿主只按 JS 给出的描述构建原生菜单, 点击(或快捷键)时把菜单项 id 发回来。
// 使用中的菜单(应用菜单、托盘菜单)内容变化后自动重新发给宿主(同一轮事件循环里的多次修改合并为一次)。
// role 是由宿主执行的标准动作(退出、复制、最小化…): 标题与快捷键缺省由系统给出, 不调用 click;
// 当前平台没有的 role(如 Windows 上的 "hide")连同菜单项一起不显示。
// 宿主没有注册 momo/menu(headless 测试运行器)时方法是空操作。
import { reportError } from "../foundation/assert";
import { isApplePlatform } from "../foundation/platform";
import { parseAccelerator, type Accelerator } from "./accelerator";
import { invokeHost } from "./host_invoke";
import type { MethodCall } from "./message_codecs";
import { MethodChannel } from "./platform_channel";
import { Widget } from "./widget";

export type MenuItemType = "normal" | "separator" | "submenu" | "checkbox" | "radio";

/**
 * 由宿主执行的标准动作。
 *  - 应用(macOS): about、services、hide、hideOthers、unhide;quit(全平台)
 *  - 编辑: undo、redo(作用于原生视图, 如网页)、cut、copy、paste、selectAll(作用于获得焦点的文本)
 *  - 窗口: minimize、zoom(最大化/还原)、close;front(macOS: 全部窗口前置)
 *  - 整个子菜单: appMenu(macOS)、editMenu、windowMenu —— 不给 submenu 时使用标准内容
 */
export type MenuItemRole =
  | "about" | "services" | "hide" | "hideOthers" | "unhide" | "quit"
  | "undo" | "redo" | "cut" | "copy" | "paste" | "selectAll"
  | "minimize" | "zoom" | "close" | "front"
  | "appMenu" | "editMenu" | "windowMenu";

export interface MenuItemOptions {
  /** 应用自己的标识, 供 {@link Menu.getMenuItemById} 查找。 */
  id?: string;
  label?: string;
  /** 缺省: 有 submenu(或 role 是整个子菜单)为 "submenu", 否则 "normal"。 */
  type?: MenuItemType;
  role?: MenuItemRole;
  /** 快捷键, 如 "CmdOrCtrl+Shift+N"(见 {@link parseAccelerator})。只有应用菜单里的快捷键会被识别。 */
  accelerator?: string;
  /** 缺省 true。 */
  enabled?: boolean;
  /** 缺省 true。false: 不出现在菜单里。 */
  visible?: boolean;
  /** checkbox / radio 的选中状态。 */
  checked?: boolean;
  /** 点击(或按下快捷键)时调用。widget: 此刻获得焦点的窗口。role 菜单项由宿主执行, 不调用 click。 */
  click?: (item: MenuItem, widget: Widget | null) => void;
  submenu?: Menu | readonly (MenuItemOptions | MenuItem)[];
}

type RoleTemplate = readonly MenuItemOptions[];
const separator: MenuItemOptions = { type: "separator" };

/** role 为整个子菜单时的标准内容。 */
function roleSubmenu(role: MenuItemRole | undefined): RoleTemplate | null {
  switch (role) {
    case "appMenu":
      return [{ role: "about" }, separator, { role: "services" }, separator,
              { role: "hide" }, { role: "hideOthers" }, { role: "unhide" }, separator, { role: "quit" }];
    case "editMenu":
      return [{ role: "undo" }, { role: "redo" }, separator,
              { role: "cut" }, { role: "copy" }, { role: "paste" }, { role: "selectAll" }];
    case "windowMenu":
      return isApplePlatform()
        ? [{ role: "minimize" }, { role: "zoom" }, separator, { role: "front" }]
        : [{ role: "minimize" }, { role: "zoom" }, { role: "close" }];
    case "services":
      return [];  // 内容由系统提供
    default:
      return null;
  }
}

let nextCommandId = 1;

/** 一个菜单项。label / enabled / visible / checked 可以随时修改, 使用中的菜单随之更新。 */
export class MenuItem {
  readonly id: string | undefined;
  readonly type: MenuItemType;
  readonly role: MenuItemRole | undefined;
  readonly accelerator: string | undefined;
  readonly submenu: Menu | null;
  click: ((item: MenuItem, widget: Widget | null) => void) | undefined;
  /** 进程内唯一的编号: 宿主以它报告点击。 */
  readonly commandId = nextCommandId++;
  /** @internal 所在的菜单(一个菜单项只能属于一个菜单)。 */
  _menu: Menu | null = null;

  readonly #accelerator: Accelerator | null;
  #label: string;
  #enabled: boolean;
  #visible: boolean;
  #checked: boolean;

  constructor(options: MenuItemOptions) {
    const submenu = options.submenu ?? roleSubmenu(options.role);
    this.id = options.id;
    this.role = options.role;
    this.type = options.type ?? (submenu !== null && submenu !== undefined ? "submenu" : "normal");
    this.accelerator = options.accelerator;
    this.#accelerator = options.accelerator === undefined ? null : parseAccelerator(options.accelerator);
    this.click = options.click;
    this.#label = options.label ?? "";
    this.#enabled = options.enabled ?? true;
    this.#visible = options.visible ?? true;
    this.#checked = options.checked ?? false;
    if (this.type === "submenu") {
      this.submenu = submenu instanceof Menu ? submenu : Menu.buildFromTemplate(submenu ?? []);
      if (this.submenu._parent !== null) throw new Error("This Menu is already the submenu of another item");
      this.submenu._parent = this;
    } else {
      if (submenu !== null && submenu !== undefined) throw new TypeError(`A "${this.type}" item cannot have a submenu`);
      this.submenu = null;
    }
  }

  /** 标题。role 菜单项没有给标题时为 ""(由系统给出)。 */
  get label(): string {
    return this.#label;
  }
  set label(value: string) {
    if (value === this.#label) return;
    this.#label = value;
    this._menu?._changed();
  }
  get enabled(): boolean {
    return this.#enabled;
  }
  set enabled(value: boolean) {
    if (value === this.#enabled) return;
    this.#enabled = value;
    this._menu?._changed();
  }
  get visible(): boolean {
    return this.#visible;
  }
  set visible(value: boolean) {
    if (value === this.#visible) return;
    this.#visible = value;
    this._menu?._changed();
  }
  /** checkbox / radio 的选中状态。选中一个 radio 会取消同组(相邻的 radio)其他项。 */
  get checked(): boolean {
    return this.#checked;
  }
  set checked(value: boolean) {
    if (value === this.#checked) return;
    this.#checked = value;
    if (value && this.type === "radio") this._menu?._uncheckRadioGroup(this);
    this._menu?._changed();
  }

  /** @internal 用户点击(或快捷键): 更新选中状态, 调用 click。 */
  _activate(): void {
    if (!this.#enabled) return;
    if (this.type === "checkbox") this.checked = !this.#checked;
    else if (this.type === "radio") this.checked = true;
    try {
      this.click?.(this, Widget.getFocused());
    } catch (e) {
      reportError(e, "in a MenuItem click handler");
    }
  }

  /** @internal 发给宿主的描述。 */
  _serialize(): Record<string, unknown> {
    const out: Record<string, unknown> = {
      id: this.commandId, type: this.type, label: this.#label, enabled: this.#enabled, checked: this.#checked,
    };
    if (this.role !== undefined) out["role"] = this.role;
    if (this.#accelerator !== null) out["accelerator"] = { ...this.#accelerator };
    if (this.submenu !== null) out["submenu"] = this.submenu._serialize();
    return out;
  }
}

const channel = new MethodChannel("momo/menu");
/** 使用中的菜单(应用菜单、托盘菜单、打开着的上下文菜单)→ 使用次数。宿主报告的点击在这里找菜单项。 */
const liveMenus = new Map<Menu, number>();
let handlerInstalled = false;
/** 打开着的上下文菜单: 弹出编号 → 宿主报告关闭时调用。 */
const openPopups = new Map<number, () => void>();
let nextPopupId = 1;

function retain(menu: Menu): void {
  if (!handlerInstalled) {
    handlerInstalled = true;
    channel.setMethodCallHandler(handleHostCall);
  }
  liveMenus.set(menu, (liveMenus.get(menu) ?? 0) + 1);
}

function release(menu: Menu): void {
  const count = (liveMenus.get(menu) ?? 0) - 1;
  if (count > 0) liveMenus.set(menu, count);
  else liveMenus.delete(menu);
}

function handleHostCall(call: MethodCall): null {
  const id = (call.args as Record<string, unknown> | null)?.["id"];
  if (call.method === "popupClosed") {
    if (typeof id === "number") openPopups.get(id)?.();
  } else if (call.method === "itemClicked") {
    for (const menu of liveMenus.keys()) {
      const item = typeof id === "number" ? menu._findCommand(id) : null;
      if (item !== null) {
        item._activate();
        break;
      }
    }
  }
  return null;
}

let applicationMenu: Menu | null = null;
let unbindApplicationMenu: (() => void) | null = null;

/** 一个菜单: 应用菜单、子菜单、上下文菜单或托盘菜单。 */
export class Menu {
  readonly #items: MenuItem[] = [];
  /** 使用方(应用菜单、托盘): 内容变化时把最新的描述发给宿主。 */
  readonly #bindings = new Set<(items: unknown[]) => void>();
  #flushScheduled = false;
  /** @internal 以本菜单为子菜单的菜单项。 */
  _parent: MenuItem | null = null;

  static buildFromTemplate(template: readonly (MenuItemOptions | MenuItem)[]): Menu {
    const menu = new Menu();
    for (const entry of template) menu.append(entry instanceof MenuItem ? entry : new MenuItem(entry));
    return menu;
  }

  /**
   * 设置应用菜单。macOS: 屏幕顶部的菜单栏, 第一项是应用菜单(通常 `{ role: "appMenu" }`);Windows: 每个窗口的
   * 菜单栏(隐藏了标题栏的窗口不显示, 快捷键照常生效)。null: 恢复宿主的默认菜单(macOS 只有隐藏与退出;
   * Windows 没有菜单栏)。
   */
  static setApplicationMenu(menu: Menu | null): void {
    if (menu === applicationMenu) return;
    unbindApplicationMenu?.();
    unbindApplicationMenu = null;
    applicationMenu = menu;
    if (menu === null) void invokeHost(channel, "setApplicationMenu", { items: null });
    else unbindApplicationMenu = menu._bind((items) => void invokeHost(channel, "setApplicationMenu", { items }));
  }
  static getApplicationMenu(): Menu | null {
    return applicationMenu;
  }

  get items(): readonly MenuItem[] {
    return this.#items;
  }
  append(item: MenuItem): void {
    this.insert(this.#items.length, item);
  }
  insert(position: number, item: MenuItem): void {
    if (item._menu !== null) throw new Error("This MenuItem already belongs to a Menu");
    item._menu = this;
    this.#items.splice(position, 0, item);
    this._changed();
  }
  /** 按 MenuItemOptions.id 查找(含子菜单)。 */
  getMenuItemById(id: string): MenuItem | null {
    return this.#find((item) => item.id === id);
  }

  /**
   * 作为上下文菜单弹出, 菜单关闭后完成。位置是 widget 内容区里的逻辑坐标;不给位置时在鼠标指针处,
   * 不给 widget 时是获得焦点的窗口。弹出的是调用时的内容。
   */
  async popup(options: { widget?: Widget; x?: number; y?: number } = {}): Promise<void> {
    const id = nextPopupId++;
    const args: Record<string, unknown> = { id, items: this._serialize() };
    const widget = options.widget ?? Widget.getFocused();
    if (widget !== null && !widget.isDestroyed()) args["view"] = widget.id;
    if (options.x !== undefined && options.y !== undefined) {
      args["x"] = options.x;
      args["y"] = options.y;
    }
    retain(this);
    try {
      const closed = new Promise<void>((resolve) => openPopups.set(id, resolve));
      // 宿主回复 true = 菜单已交给系统弹出, 关闭时另行通知(popupClosed);其余情况(宿主不支持)没有菜单。
      if ((await invokeHost<boolean>(channel, "popup", args)) === true) await closed;
    } finally {
      openPopups.delete(id);
      release(this);
    }
  }
  /** 关闭打开着的上下文菜单。 */
  closePopup(): void {
    void invokeHost(channel, "closePopup");
  }

  /** @internal 内容变了: 通知到根菜单的使用方(同一轮事件循环里合并为一次)。 */
  _changed(): void {
    const owner = this._parent?._menu;
    if (owner !== undefined && owner !== null) {
      owner._changed();
      return;
    }
    if (this.#bindings.size === 0 || this.#flushScheduled) return;
    this.#flushScheduled = true;
    void Promise.resolve().then(() => {
      this.#flushScheduled = false;
      const items = this._serialize();
      for (const push of [...this.#bindings]) push(items);
    });
  }

  /** @internal 作为一处原生菜单使用: 立即发送一次, 之后内容变化时再发。返回解除函数。 */
  _bind(push: (items: unknown[]) => void): () => void {
    this.#bindings.add(push);
    retain(this);
    push(this._serialize());
    return () => {
      this.#bindings.delete(push);
      release(this);
    };
  }

  /** @internal 发给宿主的描述(不含 visible 为 false 的菜单项)。 */
  _serialize(): unknown[] {
    return this.#items.filter((item) => item.visible).map((item) => item._serialize());
  }

  /** @internal */
  _findCommand(commandId: number): MenuItem | null {
    return this.#find((item) => item.commandId === commandId);
  }

  /** @internal 同组的 radio(与 item 相邻的连续 radio)取消选中。 */
  _uncheckRadioGroup(item: MenuItem): void {
    const index = this.#items.indexOf(item);
    for (const step of [-1, 1]) {
      for (let i = index + step; this.#items[i]?.type === "radio"; i += step) this.#items[i]!.checked = false;
    }
  }

  #find(match: (item: MenuItem) => boolean): MenuItem | null {
    for (const item of this.#items) {
      if (match(item)) return item;
      const found = item.submenu === null ? null : item.submenu.#find(match);
      if (found !== null) return found;
    }
    return null;
  }
}
