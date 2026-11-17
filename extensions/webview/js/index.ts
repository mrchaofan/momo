// momo:webview —— 内置扩展: 系统 WebView 作为平台视图(docs/07 §1)。运行时自带, 应用 `import … from "momo:webview"`。
//   WebView / WebViewController   网页视图与对它的操作
//   registerProtocol              自定义资源协议(app://… 之类), 用它加载应用自己的网页
//   WebSession / CookieStore      会话(数据分区)与 cookie
// 原生侧(native/src/, 一个 Node 插件: 平台无关的 webview_extension.cc + macOS WKWebView / Windows WebView2)注册平台视图
// 类型 "momo.webview";本文件是它唯一的调用方。
// 构建产物是 <resources>/builtin/webview/{package.json, index.js, ext_webview.node}: 本文件编译成 index.js,
// 插件就在它旁边——下面的相对导入按这个布局写。框架(momo:framework)同样由运行时提供, 两者是同一份。
// 与原生侧之间的命令、事件、协议帧的格式见 webview_extension.cc 文件头;接口设计与平台映射见 README。
import {
  FocusNode, PlatformView, State, StatefulView, utf8Decode, utf8Encode,
  type BuildContext, type Key, type PlatformViewController, type View,
} from "momo:framework";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { assetsDir, nativeHost } from "momo:services";
// @ts-ignore 编译产物旁边的原生插件: 源码树里没有这个文件, 也没有类型(形状见下)。
import addon from "./ext_webview.node";

const native = addon as {
  attach(host: bigint, options: {
    onEvent(json: string): void;
    onProtocolRequest(frame: ArrayBuffer): void;
  }): void;
  command(json: string): Promise<string>;
  respondToProtocolRequest(frame: ArrayBuffer): void;
};

export const kWebViewType = "momo.webview";

async function call(target: { id: number } | { session: string } | null, method: string, args: Record<string, unknown> = {}): Promise<unknown> {
  const reply = JSON.parse(await native.command(JSON.stringify({ ...target, method, ...args }))) as Record<string, unknown>;
  if (typeof reply.error === "string") throw new Error(`momo.webview ${method}: ${reply.error}`);
  return reply.result ?? null;
}

// ---------------------------------------------------------------------------
// 自定义资源协议

/** 页面对自定义协议地址发出的一次请求。 */
export interface ProtocolRequest {
  readonly url: string;
  readonly method: string;
  readonly headers: Readonly<Record<string, string>>;
  readonly body: Uint8Array | null;
  /** 发出请求的 WebView。 */
  readonly viewId: number;
}

/** 协议处理函数的应答。 */
export interface ProtocolResponse {
  /** 默认 200。 */
  status?: number;
  headers?: Record<string, string>;
  /** Content-Type 的简写(headers 里已有 Content-Type 时忽略)。字符串 body 默认 "text/html"。 */
  mimeType?: string;
  body?: string | Uint8Array | null;
}

export type ProtocolHandler = (request: ProtocolRequest) => ProtocolResponse | Promise<ProtocolResponse>;

/** 把协议映射到应用打包的资源目录(一个读文件的处理函数的简写)。 */
export interface AssetProtocol {
  /** 资源目录, 相对应用的 assets/(如 "web" → assets/web/)。 */
  assets: string;
  /** 目录地址对应的文件, 默认 "index.html"。 */
  index?: string;
  /** 找不到文件时改用的文件(单页应用的前端路由, 通常是 "index.html")。默认返回 404。 */
  fallback?: string;
}

type ProtocolSource = ProtocolHandler | AssetProtocol;

// 系统 WebView 自己处理、不能接管的协议。
const kBuiltInSchemes = new Set(["http", "https", "file", "about", "data", "blob", "javascript", "ws", "wss", "ftp"]);
const protocols = new Map<string, ProtocolHandler>();

const kMimeTypes: Readonly<Record<string, string>> = {
  html: "text/html", htm: "text/html", js: "text/javascript", mjs: "text/javascript", css: "text/css",
  json: "application/json", map: "application/json", txt: "text/plain", xml: "application/xml",
  svg: "image/svg+xml", png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif",
  webp: "image/webp", ico: "image/x-icon", avif: "image/avif", wasm: "application/wasm",
  woff: "font/woff", woff2: "font/woff2", ttf: "font/ttf", otf: "font/otf",
  mp3: "audio/mpeg", mp4: "video/mp4", webm: "video/webm", pdf: "application/pdf",
};

/** 从应用的资源目录应答: 目录地址取 index, 找不到文件时改用 fallback(没有则 404)。读不到资源目录之外的文件。 */
function assetHandler(source: AssetProtocol): ProtocolHandler {
  const root = path.resolve(assetsDir, source.assets);
  const read = async (file: string): Promise<Uint8Array | null> => {
    const full = path.resolve(root, file);
    if (!full.startsWith(root + path.sep)) return null;  // 越出资源目录(.. 或绝对路径)
    try {
      return await readFile(full);
    } catch {
      return null;
    }
  };
  return async (request) => {
    let file: string;
    try {
      file = decodeURIComponent(new URL(request.url).pathname).replace(/^\/+/, "");
    } catch {
      return { status: 400, mimeType: "text/plain", body: "Bad Request" };
    }
    if (file === "" || file.endsWith("/")) file += source.index ?? "index.html";
    let body = await read(file);
    if (body === null && source.fallback !== undefined) {
      file = source.fallback;
      body = await read(file);
    }
    if (body === null) return { status: 404, mimeType: "text/plain", body: "Not Found" };
    return { mimeType: kMimeTypes[path.extname(file).slice(1).toLowerCase()] ?? "application/octet-stream", body };
  };
}

/**
 * 声明一个自定义资源协议, 之后创建的 WebView 可以加载 `<scheme>://…` 地址(如 `app://localhost/index.html`)。
 * 这类地址是真实地址: 刷新、前进后退、相对路径、fetch 都有效。同一协议下 host 不同即不同源。
 *
 * 必须在创建 WebView 之前调用(通常在应用启动时): 协议集合在 WebView 创建时确定, 已创建的 WebView 看不到
 * 之后新增的协议。对已声明的协议再次调用会替换其处理函数。
 */
export function registerProtocol(scheme: string, source: ProtocolSource): void {
  if (!/^[a-z][a-z0-9+.-]*$/.test(scheme)) throw new Error(`momo.webview: invalid protocol scheme "${scheme}" (lowercase letters, digits, + . -)`);
  if (kBuiltInSchemes.has(scheme)) throw new Error(`momo.webview: "${scheme}" is handled by the system WebView and cannot be registered`);
  const declared = protocols.has(scheme);
  protocols.set(scheme, typeof source === "function" ? source : assetHandler(source));
  // 原生侧可能要在启动系统 WebView 之前知道全部协议(WebView2 只在启动时登记)。
  if (!declared) call(null, "declareProtocol", { scheme }).catch(() => {});
}

/**
 * 创建参数里的协议表。only 给出时只包含其中的协议。
 * 这里不做校验: 注册协议与打开网页是两回事, 没有注册(或不给这个视图用)的协议, 其地址加载失败, 经 onError 报告。
 */
function protocolSpecs(only?: readonly string[]): Record<string, unknown>[] {
  return [...protocols.keys()]
    .filter((scheme) => only === undefined || only.includes(scheme))
    .map((scheme) => ({ scheme }));
}

// 协议请求与应答的帧: uint32 LE 头长度 + 头 JSON + 正文。
function encodeFrame(header: unknown, body: Uint8Array | null): ArrayBuffer {
  const json = utf8Encode(JSON.stringify(header));
  const frame = new Uint8Array(4 + json.length + (body?.length ?? 0));
  new DataView(frame.buffer).setUint32(0, json.length, true);
  frame.set(json, 4);
  if (body !== null) frame.set(body, 4 + json.length);
  return frame.buffer;
}

function decodeFrame(frame: ArrayBuffer): { header: Record<string, unknown>; body: Uint8Array } {
  const length = new DataView(frame).getUint32(0, true);
  const bytes = new Uint8Array(frame);
  return {
    header: JSON.parse(utf8Decode(bytes.subarray(4, 4 + length))) as Record<string, unknown>,
    body: bytes.slice(4 + length),
  };
}

async function serveProtocolRequest(frame: ArrayBuffer): Promise<void> {
  const { header, body } = decodeFrame(frame);
  const requestId = header.requestId;
  const url = String(header.url ?? "");
  let response: ProtocolResponse;
  try {
    const handler = protocols.get(url.slice(0, url.indexOf(":")).toLowerCase());
    if (handler === undefined) {
      response = { status: 404, mimeType: "text/plain", body: "No handler for this protocol" };
    } else {
      response = await handler({
        url,
        method: String(header.method ?? "GET"),
        headers: (header.headers ?? {}) as Record<string, string>,
        body: body.length > 0 ? body : null,
        viewId: Number(header.viewId),
      });
    }
  } catch (error) {
    response = { status: 500, mimeType: "text/plain", body: String(error) };
  }
  const headers: Record<string, string> = { ...response.headers };
  if (!Object.keys(headers).some((name) => name.toLowerCase() === "content-type")) {
    const mime = response.mimeType ?? (typeof response.body === "string" ? "text/html" : "application/octet-stream");
    headers["Content-Type"] = /^text\/|json|xml/.test(mime) && !mime.includes("charset") ? `${mime}; charset=utf-8` : mime;
  }
  const bytes = typeof response.body === "string" ? utf8Encode(response.body) : response.body ?? null;
  native.respondToProtocolRequest(encodeFrame({ requestId, status: response.status ?? 200, headers }, bytes));
}

// ---------------------------------------------------------------------------
// 会话与 cookie

export interface Cookie {
  name: string;
  value: string;
  /** 以 "." 开头表示对子域也有效。 */
  domain: string;
  path: string;
  /** 过期时间(Unix 秒);null 为会话 cookie(随会话结束失效)。 */
  expires: number | null;
  secure: boolean;
  httpOnly: boolean;
  sameSite: "lax" | "strict" | null;
}

/** 要写入的 cookie: domain 与 url 至少给一个(给 url 时 domain 取其主机名)。 */
export interface CookieInit {
  name: string;
  value: string;
  url?: string;
  domain?: string;
  /** 默认 "/"。 */
  path?: string;
  expires?: number | null;
  secure?: boolean;
  httpOnly?: boolean;
  sameSite?: "lax" | "strict" | null;
}

/** 一个会话的 cookie。不依赖 WebView: 创建 WebView 之前就能预置(如登录态)。 */
export class CookieStore {
  /** @internal */
  constructor(private readonly session: WebSession) {}

  /** 全部 cookie;给出 url 时只返回会随该地址的请求发送的那些。 */
  async get(url?: string): Promise<Cookie[]> {
    return ((await call({ session: this.session.id }, "cookies.get", { url })) ?? []) as Cookie[];
  }
  /** 写入(同名同域同路径则覆盖)。 */
  async set(cookie: CookieInit): Promise<void> {
    await call({ session: this.session.id }, "cookies.set", { cookie });
  }
  /** 删除名为 name 的 cookie, 可用 url 或 domain / path 限定范围。返回删除的个数。 */
  async delete(name: string, scope: { url?: string; domain?: string; path?: string } = {}): Promise<number> {
    return Number(await call({ session: this.session.id }, "cookies.delete", { name, ...scope }));
  }
  /** 清空本会话的全部 cookie(同 `session.clearData(["cookies"])`)。 */
  clear(): Promise<void> {
    return this.session.clearData(["cookies"]);
  }
}

/** 网站数据的类别(用于 {@link WebSession.clearData})。 */
export type WebDataKind = "cookies" | "cache" | "storage";

/**
 * 会话: 一份相互隔离的网站数据(cookie、缓存、localStorage / IndexedDB…)。使用同一会话的 WebView 共享登录态,
 * 不同会话互不可见。WebView 的会话在创建时确定, 之后不能更换。
 */
export class WebSession {
  static #ephemeralCount = 0;

  /** 默认会话: 持久化到磁盘, 未指定 session 的 WebView 都用它。 */
  static readonly default = new WebSession("default");

  /** 命名的持久会话(多账号、多工作区): 同名即同一份数据, 重启后仍在。macOS 需要 14 及以上。 */
  static persistent(name: string): WebSession {
    if (name === "") throw new Error("momo.webview: a persistent session needs a name");
    return new WebSession(`persistent:${name}`);
  }

  /** 新建一个无痕会话: 数据只在内存里, 每次调用都是独立的一份。用完后 {@link dispose}。 */
  static ephemeral(): WebSession {
    return new WebSession(`ephemeral:${++WebSession.#ephemeralCount}`);
  }

  readonly cookies: CookieStore;

  /** id 相同即同一个会话(对象本身不带状态)。 */
  private constructor(readonly id: string) {
    this.cookies = new CookieStore(this);
  }

  get isPersistent(): boolean {
    return !this.id.startsWith("ephemeral:");
  }

  /** 清除网站数据;不给 kinds 则清除全部类别。 */
  async clearData(kinds?: WebDataKind[]): Promise<void> {
    await call({ session: this.id }, "session.clearData", { kinds });
  }

  /** 释放无痕会话(对持久会话无效果)。仍在使用它的 WebView 不受影响, 它们销毁后数据才真正消失。 */
  async dispose(): Promise<void> {
    if (!this.isPersistent) await call({ session: this.id }, "session.dispose");
  }
}

// ---------------------------------------------------------------------------
// WebView

/** WebView 当前状态(原生推送)。 */
export interface WebViewState {
  url: string;
  title: string;
  loading: boolean;
  /** 0..1 */
  progress: number;
  canGoBack: boolean;
  canGoForward: boolean;
}

/** 页面消息的发送方。 */
export interface MessageSource {
  /** 发送方的源, 如 "app://localhost"、"https://example.com";不透明源(loadHtml 且没有 baseUrl)为 "null"。 */
  readonly origin: string;
}

const controllers = new Map<number, WebViewController>();

native.attach(nativeHost, {
  onEvent(json) {
    const m = JSON.parse(json) as Record<string, unknown>;
    if (typeof m.id === "number") controllers.get(m.id)?._handleEvent(m);
  },
  onProtocolRequest(frame) {
    void serveProtocolRequest(frame);
  },
});

/** 操作一个已创建的 WebView。 */
export class WebViewController {
  #state: WebViewState = { url: "", title: "", loading: false, progress: 0, canGoBack: false, canGoForward: false };
  /** 状态变化(URL、标题、加载进度、前进后退能力)。 */
  onStateChanged: ((state: WebViewState) => void) | null = null;
  /**
   * 页面调用了 `window.momo.postMessage(value)`;data 是该值(经 JSON 传递)。
   * WebView 里可能是任何网站: 处理前用 source.origin 确认消息来自自己的页面(如 "app://localhost")。
   */
  onMessage: ((data: unknown, source: MessageSource) => void) | null = null;
  /** 导航失败。 */
  onError: ((message: string) => void) | null = null;
  /** 页面请求打开新窗口(target="_blank"、window.open)。WebView 自己不会打开, 由应用决定去向。 */
  onNewWindowRequested: ((url: string) => void) | null = null;

  /**
   * focusNode: 这个 WebView 在框架焦点树里的节点。系统的键盘焦点跟随它: 它持有焦点 = 键盘焦点在网页里。
   */
  constructor(readonly platformView: PlatformViewController, readonly session: WebSession,
              readonly focusNode: FocusNode = new FocusNode({ debugLabel: "WebView" })) {}

  get viewId(): number {
    return this.platformView.viewId;
  }
  get state(): WebViewState {
    return this.#state;
  }

  async loadUrl(url: string): Promise<void> {
    await call({ id: this.viewId }, "loadUrl", { url });
  }
  /** 直接加载一段 HTML。它没有真实地址(刷新会丢失): 应用自己的页面优先用 {@link registerProtocol}。 */
  async loadHtml(html: string, baseUrl?: string): Promise<void> {
    await call({ id: this.viewId }, "loadHtml", { html, baseUrl });
  }
  async goBack(): Promise<void> {
    await call({ id: this.viewId }, "goBack");
  }
  async goForward(): Promise<void> {
    await call({ id: this.viewId }, "goForward");
  }
  async reload(): Promise<void> {
    await call({ id: this.viewId }, "reload");
  }
  async stop(): Promise<void> {
    await call({ id: this.viewId }, "stop");
  }
  /** 在页面中执行脚本, 返回其结果(可 JSON 化的值;其余转为字符串)。 */
  evaluateJavaScript(script: string): Promise<unknown> {
    return call({ id: this.viewId }, "evaluateJavaScript", { script });
  }
  /**
   * 页内查找: 选中并滚动到 text 的下一处匹配(backwards: 上一处), 到头回绕;返回是否找到。从当前选区继续,
   * 所以用同一个词重复调用就是"查找下一个"。text 为空时不查找。
   */
  async find(text: string, options: { backwards?: boolean; caseSensitive?: boolean } = {}): Promise<boolean> {
    if (text === "") return false;
    return (await call({ id: this.viewId }, "find", { text, ...options })) === true;
  }
  /**
   * 向页面发消息(可 JSON 化的值)。页面里: `window.momo.addEventListener("message", (e) => e.data)`。
   * 页面尚未加载完时发出的消息会丢失。
   */
  async postMessage(data: unknown): Promise<void> {
    const json = JSON.stringify(data === undefined ? null : data);
    await this.evaluateJavaScript(`window.momo&&window.momo.dispatchEvent(new MessageEvent("message",{data:${json}}));void 0`);
  }
  /**
   * 把键盘焦点交给网页。网页内部的焦点由它自己保持: 之前聚焦的元素(输入框、选区)恢复原状, 所以 blur() 之后
   * 再 focus()、或标签被隐藏后重新显示, 用户都回到原来的位置。
   * 自绘界面里只有输入框会在点击时取得焦点;按钮等不需要键盘的组件不会动网页的焦点。其余时机(切换标签、
   * 关闭浮层之后把焦点还给网页)由应用用这两个方法决定。
   */
  focus(): void {
    this.focusNode.requestFocus();
  }
  /** 网页交出键盘焦点(回到窗口的自绘内容)。焦点不在网页里时什么也不做。 */
  blur(): void {
    this.focusNode.unfocus();
  }
  /** 键盘焦点是否在网页里。 */
  get hasFocus(): boolean {
    return this.focusNode.hasFocus;
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
      case "message": {
        const body = String(m.body ?? "null");
        let data: unknown = body;
        try {
          data = JSON.parse(body);
        } catch {
          // 页面绕过 window.momo 直接发了非 JSON 的字符串: 原样给出。
        }
        this.onMessage?.(data, { origin: String(m.origin ?? "null") });
        break;
      }
      case "error":
        this.onError?.(String(m.message ?? ""));
        break;
      case "newWindow":
        this.onNewWindowRequested?.(String(m.url ?? ""));
        break;
    }
  }
}

/** WebView 参数。除回调外都只在创建时生效;initialUrl 与 initialHtml 二选一, 都不给时为空白页。 */
export interface WebViewOptions {
  key?: Key | null;
  initialUrl?: string;
  initialHtml?: string;
  baseUrl?: string;
  /** 会话(数据分区), 默认 {@link WebSession.default}。 */
  session?: WebSession;
  /**
   * 这个 WebView 可用的自定义协议(已用 {@link registerProtocol} 声明的)。默认全部。
   * 加载不受信任网站的 WebView 应当收窄: 页面可以向它可用的任何协议发请求。
   * 不在其中(或没有声明过)的协议, 其地址打不开: 导航失败经 {@link WebViewController.onError} 报告。
   */
  protocols?: string[];
  /** 每个文档开始加载时、页面脚本之前在主框架执行的脚本(注入全局对象、打补丁)。 */
  initScripts?: string[];
  /** 覆盖 User-Agent。 */
  userAgent?: string;
  /** 允许用系统的网页检查器调试(macOS: Safari 的"开发"菜单;Windows: F12 / 右键"检查")。默认关闭。 */
  devtools?: boolean;
  onCreated?: ((controller: WebViewController) => void) | null;
  onCreateError?: ((error: unknown) => void) | null;
  placeholder?: View | null;
  focusNode?: FocusNode | null;
}

/** 系统 WebView(macOS: WKWebView;Windows: WebView2), 作为平台视图嵌入。尺寸取约束最大值。 */
export class WebView extends StatefulView {
  constructor(readonly opts: WebViewOptions = {}) {
    super(opts.key);
  }
  createState(): State {
    return new WebViewHostState();
  }
}

class WebViewHostState extends State<WebView> {
  #controller: WebViewController | null = null;
  #internalNode: FocusNode | null = null;
  #session = WebSession.default;
  #params: Uint8Array | null = null;

  override initState(): void {
    super.initState();
    const { initialUrl, initialHtml, baseUrl, session, protocols, initScripts, userAgent, devtools } = this.view.opts;
    this.#session = session ?? WebSession.default;
    this.#params = utf8Encode(JSON.stringify({
      url: initialUrl, html: initialHtml, baseUrl, session: this.#session.id, initScripts, userAgent, devtools,
      protocols: protocolSpecs(protocols),
    }));
  }

  override dispose(): void {
    if (this.#controller !== null) controllers.delete(this.#controller.viewId);
    this.#controller = null;
    this.#internalNode?.dispose();
    super.dispose();
  }

  get #focusNode(): FocusNode {
    return this.view.opts.focusNode ?? (this.#internalNode ??= new FocusNode({ debugLabel: "WebView" }));
  }

  #onCreated = (platformView: PlatformViewController): void => {
    const controller = new WebViewController(platformView, this.#session, this.#focusNode);
    this.#controller = controller;
    controllers.set(controller.viewId, controller);
    this.view.opts.onCreated?.(controller);
  };

  build(_context: BuildContext): View {
    return new PlatformView({
      viewType: kWebViewType,
      creationParams: this.#params,
      onCreated: this.#onCreated,
      onCreateError: this.view.opts.onCreateError ?? null,
      placeholder: this.view.opts.placeholder ?? null,
      focusNode: this.#focusNode,
    });
  }
}
