// examples/webview — 多窗口、多标签浏览器(平台视图 + 多视图示例, docs/05 §9、docs/02 §2)。黑白简约风格。
//   - 标签: 新建(+ / ⌘T)、关闭(× / ⌘W)、切换(点击 / ⌃Tab、⌃⇧Tab), 插入与删除带缓动;
//   - 工具栏: 后退、前进、刷新/停止, 地址栏(⌘L 聚焦;非网址按搜索处理), 加载进度细线;
//   - 窗口: ⌘N 新窗口;每个窗口是同一个 JS 运行时驱动的一个 momo 视图(Window widget), 共享应用状态;
//   - 标签右键: 窗口内菜单(momo 绘制, 叠在网页之上)——在新窗口中打开、在系统浏览器中打开;
//   - 在新窗口中打开(类似 Codex): 把标签的 WebView(GlobalKey)移到一个单独窗口, 原生网页视图换窗口,
//     页面不刷新;原窗口的标签保留并显示占位(恢复到标签页 / 显示窗口)。单独窗口可置顶、可恢复;
//     关闭单独窗口 = 关闭该标签, 网页销毁;
//   - 标签拖拽(类似 Chromium): 标签栏内左右拖动排序(其他标签平移让位);上下拖离标签栏变成新窗口,
//     新窗口跟随鼠标, 网页随之迁移不刷新;拖到另一个窗口的标签栏上并入该窗口并可继续排序;
//     窗口只有一个标签时, 拖动标签即拖动整个窗口;把这样的窗口并入别的窗口时, 原窗口关闭, 用
//     GestureBinding.transferPointerCapture 把这次按下交给目标窗口, 拖动不中断;
//   - 每个标签一个系统 WebView。非活动标签用 Offstage 隐藏: 不参与合成(原生视图被隐藏), 但 WebView
//     对象与页面状态保留, 切回时无需重新加载。
// 注意: 键盘焦点在网页里时按键由 WebView 处理, 快捷键需先点击 momo 区域(标签栏/工具栏)。
// 构建与运行(macOS, 需先构建 momo_app_mac 与 ext_webview):
//   node tools/momo-cli/momo.mjs build examples/webview && open examples/webview/dist/MomoBrowser.app
import {
  Actions, BorderRadius, GestureBinding, PointerCancelEvent, PointerUpEvent, Border, BorderSide, BoxDecoration, CallbackAction, Color, Column, Container, EdgeInsets,
  Expanded, FocusNode, Focus, FractionallySizedBox, Align, Alignment, InputDecoration, Intent, LogicalKeyboardKey,
  MaterialApp, MediaQuery, Offstage, Positioned, Row, Shortcuts, SingleActivator, SizedBox, Stack, State,
  StatefulWidget, TextEditingController, TextField, TextSelection, ThemeData, ValueKey, ViewAnchor, Window, WindowChrome, openUrl,
  reportError, runApp,
  type BuildContext, type Offset, type Widget,
} from "@momo/framework";
import { window as mainView, type View as UiView } from "momo:ui";
import { WebView, type WebViewController, type WebViewState } from "@momo-examples/ext-webview";
import { showContextMenu } from "./context_menu";
import { Palette, TextStyles, kTabStripHeight, kTabStripTopInset, kTabStripTrafficLightsCenterY, kToolbarHeight, kTrafficLightsWidth } from "./style";
import { IconTap, Tab, TabStrip, dropIndex, tabWidthFor, type TabDrag } from "./tabs";
import { DetachedPage, DetachedPlaceholder } from "./detached";

/** 起始页的内部地址(WebView 扩展的 momo:// 页面: 真实地址, 刷新后仍是起始页)。 */
export const kNewTabUrl = "momo://newtab";

const kStartPage = `<!doctype html>
<html><head><meta charset="utf-8"><title>新标签页</title>
<style>
  html,body{height:100%;margin:0;background:#fff;color:#111;font-family:-apple-system,"PingFang SC",sans-serif}
  main{height:100%;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:28px}
  h1{font-weight:300;letter-spacing:.3em;font-size:40px;margin:0}
  nav{display:flex;gap:12px;flex-wrap:wrap;justify-content:center}
  a{color:#111;text-decoration:none;border:1px solid #e6e6e6;border-radius:6px;padding:10px 18px;font-size:14px}
  a:hover{border-color:#111}
  p{color:#8a8a8a;font-size:12px;margin:0}
</style></head>
<body><main>
  <h1>MOMO</h1>
  <nav>
    <a href="https://www.apple.com/cn/">Apple</a>
    <a href="https://www.wikipedia.org/">Wikipedia</a>
    <a href="https://github.com/">GitHub</a>
    <a href="https://developer.mozilla.org/">MDN</a>
  </nav>
  <p>⌘T 新标签 · ⌘W 关闭 · ⌘L 地址栏 · ⌘R 刷新 · ⌃Tab 切换</p>
</main></body></html>`;

/** 地址栏输入 → URL:带协议的原样使用;像域名的补 https;其余按搜索。 */
function toUrl(text: string): string | null {
  const t = text.trim();
  if (t === "") return null;
  if (/^[a-z][a-z0-9+.-]*:/i.test(t)) return t;
  if (/^localhost(:\d+)?(\/.*)?$/i.test(t)) return `http://${t}`;
  if (!/\s/.test(t) && /^[^/]+\.[a-z]{2,}(:\d+)?(\/.*)?$/i.test(t)) return `https://${t}`;
  return `https://duckduckgo.com/?q=${encodeURIComponent(t)}`;
}

/** 地址栏里显示的文字(起始页显示为空)。 */
function addressOf(url: string): string {
  return url === "about:blank" || url.startsWith(kNewTabUrl) ? "" : url;
}

class NewTabIntent extends Intent {}
class NewWindowIntent extends Intent {}
class CloseTabIntent extends Intent {}
class ReloadIntent extends Intent {}
class FocusAddressIntent extends Intent {}
class NextTabIntent extends Intent {}
class PreviousTabIntent extends Intent {}

// ---------------------------------------------------------------------------
// 应用状态: 窗口与标签

let nextWindowId = 1;

/** 指针离开标签栏上下边缘超过这个距离(逻辑点)时, 标签被拖出成窗口。 */
const kTearOffDistance = 40;
/** 拖出窗口靠近另一个窗口标签栏时的上下容差(逻辑点)。 */
const kAttachSlop = 8;

/** 一个浏览器窗口的状态。 */
export class BrowserWindow {
  readonly id = nextWindowId++;
  /** 全部标签(含正在播放删除动画的)。 */
  tabs: Tab[] = [];
  activeId = 0;
  /** 已打开的原生窗口(主窗口即 momo:ui window;其他窗口打开前为 null)。 */
  view: UiView | null;
  /** 窗口尺寸与(拖出时的)位置: 仅用于创建窗口。 */
  size = { width: 1024, height: 720 };
  pos: { x: number; y: number } | null = null;
  constructor(readonly isMain: boolean) {
    this.view = isMain ? mainView : null;
  }

  /** 窗口的屏幕框(逻辑点, 主屏左上原点)。 */
  get frame(): { x: number; y: number; width: number; height: number } {
    return this.view?.screenFrame ?? { x: this.pos?.x ?? 0, y: this.pos?.y ?? 0, ...this.size };
  }

  get live(): Tab[] {
    return this.tabs.filter((t) => !t.closing);
  }
  get active(): Tab | null {
    return this.tabs.find((t) => t.id === this.activeId) ?? null;
  }
}

/**
 * 一次标签拖动(同 Chromium TabDragController: 会话独立于任何一个标签栏)。位置用屏幕坐标表示,
 * 与窗口本身的移动无关。
 *   strip: 标签在 window 的标签栏里排序;
 *   window: 标签独占 window(拖出的新窗口, 或原本只有一个标签的窗口), 窗口跟随指针移动。
 * 指针来源 owner: 按下所在窗口的标签栏;该窗口被并走关闭时, 按下序列经 transferPointerCapture 转交给
 * 目标窗口, 由 captured 接收(owner 置为 null), 同一个会话继续。
 */
interface DragSession {
  readonly tab: Tab;
  /** 按下点相对标签左上角的偏移。 */
  readonly grab: Offset;
  owner: BrowserWindow | null;
  screen: Offset;
  window: BrowserWindow;
  mode: "strip" | "window";
  /** 已转交的按下序列(取消函数);会话结束时释放。 */
  captured: (() => void) | null;
}

/** 一个标签弹出到的单独窗口(标签仍属于原窗口 from)。 */
export class DetachedWindow {
  readonly id = nextWindowId++;
  /** 窗口已打开: WebView 渲染在这里(之前仍在原窗口, 保证迁移时窗口已就绪)。 */
  ready = false;
  view: UiView | null = null;
  pinned = false;
  constructor(readonly tab: Tab, readonly from: BrowserWindow) {}
}

class BrowserApp extends StatefulWidget {
  createState(): State {
    return new BrowserAppState();
  }
}

/** 应用级状态: 所有窗口、标签操作都在这里(标签动画的 vsync 也是它, 标签换窗口后动画仍有效)。 */
export class BrowserAppState extends State<BrowserApp> {
  readonly main = new BrowserWindow(true);
  #windows: BrowserWindow[] = [this.main];
  #detached: DetachedWindow[] = [];
  readonly #pages = new Map<BrowserWindow, BrowserPageState>();
  #drag: DragSession | null = null;

  override initState(): void {
    super.initState();
    this.openTab(this.main, null);
  }

  override dispose(): void {
    this.#drag?.captured?.();
    for (const w of this.#windows) for (const tab of w.tabs) tab.dispose();
    this.#detached = [];
    super.dispose();
  }

  get windows(): readonly BrowserWindow[] {
    return this.#windows;
  }

  /** @internal 页面状态登记(地址栏同步用)。 */
  registerPage(w: BrowserWindow, page: BrowserPageState | null): void {
    if (page === null) this.#pages.delete(w);
    else this.#pages.set(w, page);
  }

  #windowOf(tab: Tab): BrowserWindow | null {
    return this.#windows.find((w) => w.tabs.includes(tab)) ?? null;
  }

  // ---- 标签 ----

  /** 在当前标签右侧插入新标签并激活;url 为空打开起始页并聚焦地址栏。 */
  openTab(w: BrowserWindow, url: string | null): void {
    const tab = new Tab(this, url);
    const at = w.tabs.findIndex((t) => t.id === w.activeId);
    w.tabs.splice(at < 0 ? w.tabs.length : at + 1, 0, tab);
    void tab.animation.forward();
    this.selectTab(w, tab);
    if (url === null) this.#pages.get(w)?.focusAddress();
  }

  selectTab(w: BrowserWindow, tab: Tab): void {
    this.setState(() => (w.activeId = tab.id));
    this.#pages.get(w)?.syncAddress(true);
  }

  /** 关闭: 先播放删除动画(期间不可交互), 结束后移除;窗口的最后一个标签: 主窗口新开一个, 其他窗口关闭窗口。 */
  closeTab(w: BrowserWindow, tab: Tab): void {
    if (tab.closing || this.#drag?.tab === tab) return;
    const live = w.live;
    const index = live.indexOf(tab);
    tab.closing = true;
    // 弹出中的标签: 单独窗口一起关闭(网页随之销毁)。
    if (tab.detached !== null) {
      const d = tab.detached;
      tab.detached = null;
      this.#detached = this.#detached.filter((x) => x !== d);
    }
    if (tab.id === w.activeId) {
      const next = live[index + 1] ?? live[index - 1] ?? null;
      if (next !== null) this.selectTab(w, next);
    }
    if (w.live.length === 0) {
      if (w.isMain) {
        this.openTab(w, null);
      } else {
        this.closeWindow(w);
        return;
      }
    }
    this.setState();
    void tab.animation.reverse().then(() => {
      if (!this.mounted) return;
      this.setState(() => (w.tabs = w.tabs.filter((t) => t !== tab)));
      tab.dispose();
    });
  }

  cycle(w: BrowserWindow, delta: number): void {
    const live = w.live;
    if (live.length < 2) return;
    const index = live.findIndex((t) => t.id === w.activeId);
    this.selectTab(w, live[(index + delta + live.length) % live.length]!);
  }

  // ---- 窗口 ----

  /** 新窗口: url 为空打开起始页。 */
  openWindow(url: string | null): void {
    const w = new BrowserWindow(false);
    this.setState(() => this.#windows.push(w));
    this.openTab(w, url);
  }

  #onWindowOpened(w: BrowserWindow, view: UiView): void {
    if (!this.#windows.includes(w)) return;
    // 打开期间指针已移动: 补到最新位置。
    if (w.pos !== null && this.#drag?.window === w) view.setPosition(w.pos.x, w.pos.y);
    this.setState(() => {
      w.view = view;
      // 寄放在原窗口的网页同一帧移过来。
      for (const tab of w.tabs) tab.parking = null;
    });
  }

  /** 由原窗口暂时渲染(隐藏)的网页: 拖出成的新窗口尚未打开。 */
  parkedIn(w: BrowserWindow): Tab[] {
    const out: Tab[] = [];
    for (const x of this.#windows) for (const tab of x.tabs) if (tab.parking === w) out.push(tab);
    return out;
  }

  // ---- 标签拖拽(类似 Chromium)----

  /** 本窗口标签栏里正在排序的标签(浮动副本的窗口内 x)。 */
  stripDrag(w: BrowserWindow): TabDrag | null {
    const d = this.#drag;
    if (d === null || d.mode !== "strip" || d.window !== w) return null;
    return { tab: d.tab, left: this.#dragLeft(d, w) };
  }

  /** 被拖标签左边界在窗口 w 内的 x。 */
  #dragLeft(d: DragSession, w: BrowserWindow): number {
    return d.screen.dx - w.frame.x - d.grab.dx;
  }

  /** 窗口跟随指针: 标签(窗口里第一个标签位)保持在指针下的按下位置。 */
  #followPointer(d: DragSession): { x: number; y: number } {
    return { x: d.screen.dx - kTrafficLightsWidth - d.grab.dx, y: d.screen.dy - kTabStripTopInset - d.grab.dy };
  }

  dragStart(w: BrowserWindow, tab: Tab, grab: Offset, screen: Offset): void {
    if (this.#drag !== null || tab.closing || !w.tabs.includes(tab)) return;
    // 只有一个标签: 直接拖动整个窗口。
    const mode = w.live.length === 1 && tab.detached === null ? "window" : "strip";
    this.#drag = { tab, grab, owner: w, screen, window: w, mode, captured: null };
    this.selectTab(w, tab);
  }

  /** 标签栏上报的移动: 只接受指针来源窗口的(按下序列转交后, 旧窗口的取消等不影响会话)。 */
  dragUpdate(from: BrowserWindow, screen: Offset): void {
    const d = this.#drag;
    if (d !== null && d.owner === from) this.#dragTo(d, screen);
  }

  dragEnd(from: BrowserWindow): void {
    if (this.#drag !== null && this.#drag.owner === from) this.#finishDrag();
  }

  #dragTo(d: DragSession, screen: Offset): void {
    d.screen = screen;
    if (d.mode === "strip") {
      const y = screen.dy - d.window.frame.y;
      // 弹出到单独窗口的标签只能排序。
      if (d.tab.detached === null && (y < -kTearOffDistance || y > kTabStripHeight + kTearOffDistance)) this.#tearOff(d);
      this.setState();
      return;
    }
    const w = d.window;
    w.pos = this.#followPointer(d);
    w.view?.setPosition(w.pos.x, w.pos.y);
    const target = this.#attachTarget(d);
    if (target !== null) this.#attach(d, target);
  }

  /** 松手: 排序中提交落点;拖出的窗口激活。 */
  #finishDrag(): void {
    const d = this.#drag;
    if (d === null) return;
    d.captured?.();
    this.#drag = null;
    if (!this.mounted) return;
    if (d.mode === "window") {
      d.window.view?.activate();
      this.setState();
      return;
    }
    const w = d.window;
    const live = w.live;
    const from = live.indexOf(d.tab);
    const to = dropIndex(this.#dragLeft(d, w), tabWidthFor(w.frame.width, live.length), live.length);
    this.setState(() => {
      if (from < 0 || to === from) return;
      const rest = w.tabs.filter((t) => t !== d.tab);
      const anchor = live.filter((t) => t !== d.tab)[to] ?? null;
      rest.splice(anchor !== null ? rest.indexOf(anchor) : rest.length, 0, d.tab);
      w.tabs = rest;
    });
  }

  /** 拖离标签栏: 多个标签时移出到新窗口(窗口打开前网页寄放在原窗口);只有一个标签时改为拖动窗口。 */
  #tearOff(d: DragSession): void {
    const from = d.window;
    const live = from.live;
    d.mode = "window";
    if (live.length <= 1) return;
    const tab = d.tab;
    const index = live.indexOf(tab);
    if (from.activeId === tab.id) from.activeId = (live[index + 1] ?? live[index - 1])!.id;
    from.tabs = from.tabs.filter((t) => t !== tab);
    const nw = new BrowserWindow(false);
    const f = from.frame;
    if (f.width > 0 && f.height > 0) nw.size = { width: f.width, height: f.height };
    nw.tabs = [tab];
    nw.activeId = tab.id;
    tab.parking = from;
    d.window = nw;
    nw.pos = this.#followPointer(d);
    this.#windows.push(nw);
    this.#pages.get(from)?.syncAddress(true);
  }

  /** 指针所在的另一个窗口的标签栏(可并入的目标)。主窗口不能关闭, 其唯一标签不并入别处。 */
  #attachTarget(d: DragSession): BrowserWindow | null {
    const src = d.window;
    if ((src.isMain && src.live.length <= 1) || d.tab.detached !== null || d.tab.parking !== null) return null;
    for (const t of this.#windows) {
      if (t === src || t.view === null) continue;
      const f = t.frame;
      const x = d.screen.dx - f.x;
      const y = d.screen.dy - f.y;
      if (x >= 0 && x < f.width && y >= -kAttachSlop && y < kTabStripHeight + kAttachSlop) return t;
    }
    return null;
  }

  /** 并入目标窗口: 按指针位置插入标签栏并继续排序;拖出的窗口变空后关闭。网页同一帧迁移, 不刷新。 */
  #attach(d: DragSession, target: BrowserWindow): void {
    const src = d.window;
    const tab = d.tab;
    const live = target.live;
    const at = dropIndex(this.#dragLeft(d, target), tabWidthFor(target.frame.width, live.length + 1), live.length + 1);
    const anchor = live[at] ?? null;
    this.setState(() => {
      src.tabs = src.tabs.filter((t) => t !== tab);
      if (src.activeId === tab.id) src.activeId = src.live[0]?.id ?? 0;
      target.tabs.splice(anchor !== null ? target.tabs.indexOf(anchor) : target.tabs.length, 0, tab);
      target.activeId = tab.id;
      d.window = target;
      d.mode = "strip";
    });
    target.view?.activate();
    this.#pages.get(target)?.syncAddress(true);
    if (src.live.length === 0) {
      if (src === d.owner && target.view !== null) {
        // 指针来源窗口自己被并走、即将关闭: 按下序列转交给目标窗口, 本会话继续(排序或再次拖出)。
        // 没有可转交的序列(按键已松开)时结束会话。
        d.owner = null;
        d.captured = GestureBinding.instance.transferPointerCapture(target.view, (e) => {
          if (e instanceof PointerUpEvent || e instanceof PointerCancelEvent) this.#finishDrag();
          else this.#dragTo(d, e.screenPosition);
        });
        if (d.captured === null) this.#finishDrag();
      }
      this.closeWindow(src);
    }
  }

  // ---- 弹出到单独窗口 ----

  /** 把标签弹出到单独窗口(已弹出则把那个窗口带到最前)。窗口打开后 WebView 才移过去, 页面不刷新。 */
  detachTab(from: BrowserWindow, tab: Tab): void {
    if (tab.closing) return;
    if (tab.detached !== null) {
      this.showDetached(tab);
      return;
    }
    const d = new DetachedWindow(tab, from);
    tab.detached = d;
    this.setState(() => this.#detached.push(d));
  }

  #onDetachedOpened(d: DetachedWindow, view: UiView): void {
    if (!this.#detached.includes(d)) return;
    this.setState(() => {
      d.ready = true;
      d.view = view;
    });
  }

  /** 恢复到标签页: 关闭单独窗口, WebView 移回原窗口(同一帧内迁移, 不刷新)并选中该标签。 */
  restoreTab(tab: Tab): void {
    const d = tab.detached;
    if (d === null) return;
    this.setState(() => {
      tab.detached = null;
      this.#detached = this.#detached.filter((x) => x !== d);
      d.from.activeId = tab.id;
    });
    this.#pages.get(d.from)?.syncAddress(true);
  }

  showDetached(tab: Tab): void {
    tab.detached?.view?.activate();
  }

  togglePin(d: DetachedWindow): void {
    this.setState(() => (d.pinned = !d.pinned));
  }

  /** 用户关闭了单独窗口: 原窗口的标签一起关闭, 网页销毁。 */
  #onDetachedClosed(d: DetachedWindow): void {
    if (!this.#detached.includes(d)) return;
    this.setState(() => {
      d.tab.detached = null;
      this.#detached = this.#detached.filter((x) => x !== d);
    });
    this.closeTab(d.from, d.tab);
  }

  /** 关闭(非主)窗口: 其标签与网页随窗口卸载一起释放。 */
  closeWindow(w: BrowserWindow): void {
    if (w.isMain || !this.#windows.includes(w)) return;
    this.setState(() => {
      this.#windows = this.#windows.filter((x) => x !== w);
      this.#detached = this.#detached.filter((d) => d.from !== w);
    });
    for (const tab of w.tabs) tab.dispose();
  }

  // ---- 页面状态 ----

  onWebCreated(tab: Tab, web: WebViewController): void {
    tab.controller = web;
    web.onStateChanged = (s: WebViewState) => {
      if (!this.mounted) return;
      this.setState(() => tab.apply(s));
      const w = this.#windowOf(tab);
      if (w !== null && w.activeId === tab.id && tab.detached === null) this.#pages.get(w)?.syncAddress(false);
    };
  }

  build(_context: BuildContext): Widget {
    return new ViewAnchor({
      child: new BrowserPage({ app: this, model: this.main }),
      views: this.#windows.filter((w) => !w.isMain).map((w) => new Window({
        key: new ValueKey(w.id),
        title: w.active?.displayTitle ?? "Momo Browser",
        width: w.size.width,
        height: w.size.height,
        minWidth: 480,
        minHeight: 320,
        x: w.pos?.x,
        y: w.pos?.y,
        onOpened: (view) => this.#onWindowOpened(w, view),
        onClosed: () => this.closeWindow(w),
        onCreateError: (e) => {
          reportError(e, "opening a browser window");
          this.closeWindow(w);
        },
        child: new BrowserPage({ app: this, model: w }),
      })).concat(this.#detached.map((d) => new Window({
        key: new ValueKey(d.id),
        title: d.tab.displayTitle,
        width: 960,
        height: 720,
        minWidth: 420,
        minHeight: 300,
        alwaysOnTop: d.pinned,
        // 单独的单标签窗口是面板: 可接收键盘, 但不成为 main window, 聚焦它也不激活应用。
        panel: true,
        onOpened: (view) => this.#onDetachedOpened(d, view),
        onClosed: () => this.#onDetachedClosed(d),
        onCreateError: (e) => {
          reportError(e, "opening a separate window");
          this.setState(() => {
            d.tab.detached = null;
            this.#detached = this.#detached.filter((x) => x !== d);
          });
        },
        // 窗口就绪后网页才移过来(同一帧里原窗口换成占位)。
        child: new DetachedPage({ app: this, detached: d, page: d.ready ? this.webViewFor(d.tab) : new SizedBox({}) }),
      }))),
    });
  }

  /** 标签的网页(GlobalKey: 在原窗口与单独窗口之间迁移时保持同一个 WebView)。 */
  webViewFor(tab: Tab): Widget {
    return new WebView({
      key: tab.pageKey,
      initialUrl: tab.initialUrl ?? kNewTabUrl,
      pages: { [kNewTabUrl]: kStartPage },
      onCreated: (web) => this.onWebCreated(tab, web),
    });
  }
}

// ---------------------------------------------------------------------------
// 一个窗口的页面

class BrowserPage extends StatefulWidget {
  constructor(readonly o: { app: BrowserAppState; model: BrowserWindow }) {
    super();
  }
  createState(): State {
    return new BrowserPageState();
  }
}

class BrowserPageState extends State<BrowserPage> {
  #address = new TextEditingController();
  #addressFocus = new FocusNode({ debugLabel: "address" });

  get #app(): BrowserAppState {
    return this.widget.o.app;
  }
  get #w(): BrowserWindow {
    return this.widget.o.model;
  }

  override initState(): void {
    super.initState();
    this.#app.registerPage(this.#w, this);
    this.syncAddress(true);
  }

  override dispose(): void {
    this.#app.registerPage(this.#w, null);
    this.#address.dispose();
    this.#addressFocus.dispose();
    super.dispose();
  }

  /** 地址栏显示活动标签的地址(force: 切换标签时;否则只在未编辑时)。 */
  syncAddress(force: boolean): void {
    if (!force && this.#addressFocus.hasFocus) return;
    this.#address.text = addressOf(this.#w.active?.url ?? "");
  }

  focusAddress = (): void => {
    this.#addressFocus.requestFocus();
    this.#address.selection = new TextSelection(0, this.#address.text.length);
  };

  #go = (text: string): void => {
    const url = toUrl(text);
    const web = this.#w.active?.controller;
    if (url === null || !web) return;
    this.#address.text = url;
    void web.loadUrl(url);
    void web.focus();  // 键盘焦点交给网页
  };

  /** 标签右键菜单: 在新窗口中打开(同一 JS 运行时的新 momo 窗口)、在系统浏览器中打开。 */
  #showTabMenu = (tab: Tab, position: Offset): void => {
    const url = /^https?:\/\//i.test(tab.url) ? tab.url : null;
    const app = this.#app;
    showContextMenu(this.context, position, {
      title: tab.displayTitle,
      items: [
        // 起始页等非网页地址: 新窗口打开起始页。
        { label: "在新窗口中打开", onSelect: () => app.detachTab(this.#w, tab) },
        // 只有网页地址才能交给系统浏览器;否则不显示这一项。
        ...(url ? [{
          label: "在系统浏览器中打开",
          onSelect: () => void openUrl(url).catch((e: unknown) => reportError(e, "opening the system browser")),
        }] : []),
      ],
    });
  };

  #toolbar(active: Tab | null): Widget {
    const web = active?.controller ?? null;
    const loading = active?.loading ?? false;
    return new Container({
      height: kToolbarHeight,
      padding: EdgeInsets.symmetric({ horizontal: 10 }),
      decoration: new BoxDecoration({ color: Palette.white, border: new Border({ bottom: new BorderSide({ color: Palette.line, width: 1 }) }) }),
      child: new Row({
        children: [
          new IconTap({ glyph: "‹", onTap: web && active!.canGoBack ? () => void web.goBack() : null }),
          new IconTap({ glyph: "›", onTap: web && active!.canGoForward ? () => void web.goForward() : null }),
          new IconTap({ glyph: loading ? "×" : "↻", onTap: web ? () => void (loading ? web.stop() : web.reload()) : null }),
          new SizedBox({ width: 10 }),
          new Expanded({
            child: new TextField({
              controller: this.#address,
              focusNode: this.#addressFocus,
              style: TextStyles.address,
              onSubmitted: this.#go,
              decoration: new InputDecoration({
                hintText: "搜索或输入网址",
                filled: true,
                fillColor: Palette.field,
                borderRadius: BorderRadius.circular(6),
                contentPadding: EdgeInsets.symmetric({ horizontal: 12, vertical: 7 }),
              }),
            }),
          }),
          new SizedBox({ width: 6 }),
        ],
      }),
    });
  }

  /** 加载进度: 工具栏下方 2px 黑线, 宽度为进度。 */
  #progress(active: Tab | null): Widget {
    const p = active?.loading ? Math.max(0.05, active.progress) : 0;
    return new SizedBox({
      height: 2,
      child: new Align({
        alignment: Alignment.centerLeft,
        child: new FractionallySizedBox({ widthFactor: p, child: new Container({ color: p > 0 ? Palette.black : Palette.white }) }),
      }),
    });
  }

  build(context: BuildContext): Widget {
    const app = this.#app;
    const w = this.#w;
    const active = w.active;
    const width = MediaQuery.of(context).size.width;
    const tabWidth = tabWidthFor(width, w.live.length);

    const pages = new Stack({
      children: [
        ...w.live.filter((tab) => tab.parking === null).map((tab) => Positioned.fill({
          key: new ValueKey(tab.id),
          child: new Offstage({
            offstage: tab.id !== w.activeId,
            // 已弹出到单独窗口(且窗口已打开): 这里显示占位, WebView 在那个窗口里。
            child: tab.detached?.ready ? new DetachedPlaceholder({ app, tab }) : app.webViewFor(tab),
          }),
        })),
        // 拖出到新窗口、窗口尚在打开: 网页暂留在这里(隐藏), 保持挂载。
        ...app.parkedIn(w).map((tab) => Positioned.fill({ key: new ValueKey(tab.id), child: new Offstage({ offstage: true, child: app.webViewFor(tab) }) })),
      ],
    });

    const body = new Container({
      color: Palette.white,
      child: new Column({
        children: [
          new TabStrip({
            tabs: w.tabs,
            activeId: w.activeId,
            tabWidth,
            drag: app.stripDrag(w),
            onSelect: (tab) => app.selectTab(w, tab),
            onClose: (tab) => app.closeTab(w, tab),
            onContextMenu: this.#showTabMenu,
            onNew: () => app.openTab(w, null),
            onDragStart: (tab, grab, screen) => app.dragStart(w, tab, grab, screen),
            onDragUpdate: (screen) => app.dragUpdate(w, screen),
            onDragEnd: () => app.dragEnd(w),
          }),
          this.#toolbar(active),
          this.#progress(active),
          new Expanded({ child: pages }),
        ],
      }),
    });

    return new Shortcuts({
      shortcuts: new Map([
        [new SingleActivator(LogicalKeyboardKey.keyT, { meta: true }), new NewTabIntent()],
        [new SingleActivator(LogicalKeyboardKey.keyN, { meta: true }), new NewWindowIntent()],
        [new SingleActivator(LogicalKeyboardKey.keyW, { meta: true }), new CloseTabIntent()],
        [new SingleActivator(LogicalKeyboardKey.keyR, { meta: true }), new ReloadIntent()],
        [new SingleActivator(LogicalKeyboardKey.keyL, { meta: true }), new FocusAddressIntent()],
        [new SingleActivator(LogicalKeyboardKey.tab, { control: true }), new NextTabIntent()],
        [new SingleActivator(LogicalKeyboardKey.tab, { control: true, shift: true }), new PreviousTabIntent()],
      ]),
      child: new Actions({
        actions: new Map([
          [NewTabIntent, new CallbackAction<NewTabIntent>({ onInvoke: () => app.openTab(w, null) })],
          [NewWindowIntent, new CallbackAction<NewWindowIntent>({ onInvoke: () => app.openWindow(null) })],
          [CloseTabIntent, new CallbackAction<CloseTabIntent>({ onInvoke: () => { const t = w.active; if (t) app.closeTab(w, t); } })],
          [ReloadIntent, new CallbackAction<ReloadIntent>({ onInvoke: () => void w.active?.controller?.reload() })],
          [FocusAddressIntent, new CallbackAction<FocusAddressIntent>({ onInvoke: this.focusAddress })],
          [NextTabIntent, new CallbackAction<NextTabIntent>({ onInvoke: () => app.cycle(w, 1) })],
          [PreviousTabIntent, new CallbackAction<PreviousTabIntent>({ onInvoke: () => app.cycle(w, -1) })],
        ]),
        // 无边框窗口: 标签栏顶到窗口上沿, 红绿灯与标签栏垂直居中。
        child: new WindowChrome({ trafficLightsCenterY: kTabStripTrafficLightsCenterY, child: new Focus({ autofocus: true, child: body }) }),
      }),
    });
  }
}

runApp(new MaterialApp({
  title: "Momo Browser",
  theme: new ThemeData({ primaryColor: new Color(0xff111111), scaffoldBackgroundColor: Palette.white }),
  home: new BrowserApp(),
}));
