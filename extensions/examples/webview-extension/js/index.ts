// @momo-examples/ext-webview 的 JS 门面: WebView widget + WebViewController。
// 原生侧(native/src/ext_webview.mm)注册平台视图类型 "momo.webview";本文件只用框架的公开 API。
// 协议见 ext_webview.mm 文件头(JSON, 命令通道 momo.webview, 事件通道 momo.webview/events)。
import {
  BasicMessageChannel, JSONMessageCodec, PlatformView, State, StatefulWidget, utf8Encode,
  type BuildContext, type FocusNode, type Key, type PlatformViewController, type Widget,
} from "@momo/framework";

export const kWebViewType = "momo.webview";

/** WebView 当前状态(原生 KVO 推送)。 */
export interface WebViewState {
  url: string;
  title: string;
  loading: boolean;
  /** 0..1 */
  progress: number;
  canGoBack: boolean;
  canGoForward: boolean;
}

const commands = new BasicMessageChannel<unknown>("momo.webview", JSONMessageCodec.instance);
const events = new BasicMessageChannel<unknown>("momo.webview/events", JSONMessageCodec.instance);
const controllers = new Map<number, WebViewController>();
let eventsInstalled = false;

function installEvents(): void {
  if (eventsInstalled) return;
  eventsInstalled = true;
  events.setMessageHandler((message) => {
    const m = message as Record<string, unknown> | null;
    if (m === null || typeof m.id !== "number") return;
    controllers.get(m.id)?._handleEvent(m);
  });
}

/** 操作一个已创建的 WebView。 */
export class WebViewController {
  #state: WebViewState = { url: "", title: "", loading: false, progress: 0, canGoBack: false, canGoForward: false };
  /** 状态变化(URL、标题、加载进度、前进后退能力)。 */
  onStateChanged: ((state: WebViewState) => void) | null = null;
  /** 页面调用 window.webkit.messageHandlers.momo.postMessage(body)。非字符串 body 以 JSON 文本给出。 */
  onMessage: ((body: string) => void) | null = null;
  /** 导航失败。 */
  onError: ((message: string) => void) | null = null;

  constructor(readonly platformView: PlatformViewController) {}

  get viewId(): number {
    return this.platformView.viewId;
  }
  get state(): WebViewState {
    return this.#state;
  }

  loadUrl(url: string): Promise<void> {
    return this.#call("loadUrl", { url }).then(() => undefined);
  }
  loadHtml(html: string, baseUrl?: string): Promise<void> {
    return this.#call("loadHtml", { html, baseUrl }).then(() => undefined);
  }
  goBack(): Promise<void> {
    return this.#call("goBack").then(() => undefined);
  }
  goForward(): Promise<void> {
    return this.#call("goForward").then(() => undefined);
  }
  reload(): Promise<void> {
    return this.#call("reload").then(() => undefined);
  }
  stop(): Promise<void> {
    return this.#call("stop").then(() => undefined);
  }
  /** 在页面中执行脚本, 返回其结果(可 JSON 化的值;其余转为字符串)。 */
  evaluateJavaScript(script: string): Promise<unknown> {
    return this.#call("evaluateJavaScript", { script });
  }
  /** 把键盘焦点交给 WebView。 */
  focus(): Promise<void> {
    return this.platformView.focus();
  }

  async #call(method: string, args: Record<string, unknown> = {}): Promise<unknown> {
    const reply = (await commands.send({ id: this.viewId, method, ...args })) as Record<string, unknown> | null;
    if (reply === null) throw new Error("momo.webview: the WebView extension is not loaded");
    if (typeof reply.error === "string") throw new Error(`momo.webview ${method}: ${reply.error}`);
    return reply.result ?? null;
  }

  /** @internal */
  _handleEvent(m: Record<string, unknown>): void {
    switch (m.event) {
      case "state":
        this.#state = {
          url: String(m.url ?? ""),
          title: String(m.title ?? ""),
          loading: m.loading === true,
          progress: typeof m.progress === "number" ? m.progress : 0,
          canGoBack: m.canGoBack === true,
          canGoForward: m.canGoForward === true,
        };
        this.onStateChanged?.(this.#state);
        break;
      case "message":
        this.onMessage?.(String(m.body ?? ""));
        break;
      case "error":
        this.onError?.(String(m.message ?? ""));
        break;
    }
  }
}

/** WebView 参数(url 与 html 二选一;都不给时为空白页)。 */
export interface WebViewOptions {
  key?: Key | null;
  initialUrl?: string;
  initialHtml?: string;
  baseUrl?: string;
  /**
   * 内部页面: momo:// 地址 → HTML(进程内共享)。由扩展直接返回, 是真实地址——刷新、前进后退都有效。
   * 例: { initialUrl: "momo://newtab", pages: { "momo://newtab": html } }
   */
  pages?: Record<string, string>;
  onCreated?: ((controller: WebViewController) => void) | null;
  onCreateError?: ((error: unknown) => void) | null;
  placeholder?: Widget | null;
  focusNode?: FocusNode | null;
}

/** 系统 WebView(macOS: WKWebView), 作为平台视图嵌入。尺寸取约束最大值。 */
export class WebView extends StatefulWidget {
  constructor(readonly opts: WebViewOptions = {}) {
    super(opts.key);
  }
  createState(): State {
    return new WebViewWidgetState();
  }
}

class WebViewWidgetState extends State<WebView> {
  #controller: WebViewController | null = null;
  #params: Uint8Array | null = null;

  override initState(): void {
    super.initState();
    const { initialUrl, initialHtml, baseUrl, pages } = this.widget.opts;
    this.#params = utf8Encode(JSON.stringify({ url: initialUrl, html: initialHtml, baseUrl, pages }));
  }

  override dispose(): void {
    if (this.#controller !== null) controllers.delete(this.#controller.viewId);
    this.#controller = null;
    super.dispose();
  }

  #onCreated = (platformView: PlatformViewController): void => {
    installEvents();
    const controller = new WebViewController(platformView);
    this.#controller = controller;
    controllers.set(controller.viewId, controller);
    this.widget.opts.onCreated?.(controller);
  };

  build(_context: BuildContext): Widget {
    return new PlatformView({
      viewType: kWebViewType,
      creationParams: this.#params,
      onCreated: this.#onCreated,
      onCreateError: this.widget.opts.onCreateError ?? null,
      placeholder: this.widget.opts.placeholder ?? null,
      focusNode: this.widget.opts.focusNode ?? null,
    });
  }
}
