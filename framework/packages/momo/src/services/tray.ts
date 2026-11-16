// 系统托盘图标(对标 Electron Tray;协议见 embedder/common/tray_channel.h 的 momo/tray)。macOS: 菜单栏右侧的
// 状态项;Windows: 任务栏通知区域的图标。
//
//   const tray = new Tray("tray.png");                 // <应用>/assets/tray.png
//   tray.setToolTip("My App");
//   tray.setContextMenu(Menu.buildFromTemplate([{ label: "Show", click: show }, { role: "quit" }]));
//   tray.on("click", () => show());
//
// 图片: 绝对路径, 或相对应用 assets 目录的路径。macOS 用 PNG: 文件名以 "Template" 结尾(trayTemplate.png)的
// 按模板图像处理(只取 alpha, 颜色跟随菜单栏), 旁边的 "@2x" 文件(trayTemplate@2x.png)用于高分屏;
// Windows 用 .ico。
// 生命周期: 图标一直存在到 destroy()(或应用退出);JS 丢掉引用不会移除图标。
// 宿主没有注册 momo/tray(headless 测试运行器)、或平台没有托盘时方法是空操作。
import { assetsDir } from "momo:services";
import { EventEmitter } from "../foundation/event_emitter";
import { invokeHost } from "./host_invoke";
import type { Menu } from "./menu";
import type { MethodCall } from "./message_codecs";
import { MethodChannel } from "./platform_channel";
import type { WidgetBounds } from "./widget";

export interface TrayEvent {
  /** 图标在屏幕上的框(逻辑点, 主屏左上原点), 可用来在图标旁边放窗口;取不到时为 null。 */
  readonly bounds: WidgetBounds | null;
}

export type TrayEvents = {
  /** 左键点击。macOS 上设置了上下文菜单时点击直接打开菜单, 不触发。 */
  "click": [event: TrayEvent];
  /** 右键点击。设置了上下文菜单时直接打开菜单, 不触发。 */
  "right-click": [event: TrayEvent];
  "double-click": [event: TrayEvent];
};

const channel = new MethodChannel("momo/tray");
const trays = new Map<number, Tray>();
let nextId = 1;
let handlerInstalled = false;

const kHostEvents: Readonly<Record<string, keyof TrayEvents>> = {
  click: "click", rightClick: "right-click", doubleClick: "double-click",
};

/** 宿主以数字列表 [x, y, w, h] 给出矩形(解码后是 Float64Array)。 */
function toBounds(v: unknown): WidgetBounds | null {
  if (v === null || typeof v !== "object" || typeof (v as ArrayLike<number>).length !== "number") return null;
  const a = v as ArrayLike<number>;
  if (a.length < 4) return null;
  return { x: Number(a[0]), y: Number(a[1]), width: Number(a[2]), height: Number(a[3]) };
}

function resolveImage(image: string): string {
  const absolute = image.startsWith("/") || /^[A-Za-z]:[\\/]/.test(image) || image.startsWith("\\\\");
  return absolute ? image : `${assetsDir}/${image}`;
}

/** 一个托盘图标。 */
export class Tray extends EventEmitter<TrayEvents> {
  readonly #id = nextId++;
  #destroyed = false;
  #unbindMenu: (() => void) | null = null;

  constructor(image: string) {
    super();
    if (!handlerInstalled) {
      handlerInstalled = true;
      channel.setMethodCallHandler(Tray.#dispatch);
    }
    trays.set(this.#id, this);
    this.#invoke("create", { image: resolveImage(image) });
  }

  setImage(image: string): void {
    this.#invoke("setImage", { image: resolveImage(image) });
  }
  /** 鼠标悬停时的提示文字。 */
  setToolTip(text: string): void {
    this.#invoke("setToolTip", { text });
  }
  /** 图标旁边的文字(只有 macOS)。 */
  setTitle(title: string): void {
    this.#invoke("setTitle", { title });
  }
  /** 上下文菜单;null 移除。菜单内容之后的修改会自动同步。 */
  setContextMenu(menu: Menu | null): void {
    this.#unbindMenu?.();
    this.#unbindMenu = null;
    if (this.#destroyed) return;
    if (menu === null) this.#invoke("setContextMenu", { items: null });
    else this.#unbindMenu = menu._bind((items) => this.#invoke("setContextMenu", { items }));
  }
  /** 图标在屏幕上的框;取不到时为 null。 */
  async getBounds(): Promise<WidgetBounds | null> {
    if (this.#destroyed) return null;
    return toBounds(await invokeHost(channel, "getBounds", { id: this.#id }));
  }
  /** 移除图标。之后方法是空操作。 */
  destroy(): void {
    if (this.#destroyed) return;
    this.setContextMenu(null);
    this.#invoke("destroy");
    this.#destroyed = true;
    trays.delete(this.#id);
    this.removeAllListeners();
  }
  isDestroyed(): boolean {
    return this.#destroyed;
  }

  #invoke(method: string, args: Record<string, unknown> = {}): void {
    if (this.#destroyed) return;
    void invokeHost(channel, method, { id: this.#id, ...args });
  }

  static #dispatch(call: MethodCall): null {
    const args = (call.args ?? {}) as Record<string, unknown>;
    const tray = trays.get(typeof args["id"] === "number" ? args["id"] : -1);
    const event = kHostEvents[call.method];
    if (tray !== undefined && event !== undefined) tray.emit(event, { bounds: toBounds(args["bounds"]) });
    return null;
  }
}
