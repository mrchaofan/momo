# momo:webview

系统 WebView(macOS `WKWebView`)作为**平台视图**嵌入 momo 界面的**内置扩展**:随运行时提供, 应用直接
`import { WebView } from "momo:webview"`, 不需要安装或配置任何东西(docs/07 §1、§2.1)。目前只有 macOS。原理见
`docs/05-rendering-pipeline.md` §9, 扩展侧接口见 `docs/07-extension-system.md` §3。

- 原生部分 `native/src/ext_webview.mm`:一个 Node 插件。与 JS 之间用 Node-API, 与引擎之间用宿主能力表(`momo_host.h`):注册视图类型 `momo.webview`, 命令经 `post_platform_task` 到主线程, 事件经 `napi_threadsafe_function` 回 JS。线上协议在文件头。
- JS 门面 `js/index.ts`(唯一的入口):下面的全部接口。它 `import "momo:framework"`(与应用拿到的是同一份), 按相对路径 `./ext_webview.node` 加载插件。
- 构建:随运行时一起(CMake target `momo_builtin` 与 `ext_webview`), JS 编译成 `<构建目录>/embedder/builtin/webview/index.js`, 插件输出到同一个目录的 `ext_webview.node`, 再同步进 `Momo.app/Contents/Resources/builtin/webview/`(docs/09 §4)。这里没有 `package.json`, 它不是一个 npm 包。
- 类型:目前没有单独发布的类型包;仓库内靠 `framework/tsconfig.json` 的 `paths` 把 `momo:webview` 指到 `js/index.ts`。

接口参照 Tauri(wry)的取舍, 只保留 WKWebView 与 WebView2 都能实现的部分。

## 自定义资源协议

应用自己的网页不走 `file://`, 也不用 `loadHtml`, 而是声明一个协议并用它加载:

```ts
import { WebView, registerProtocol } from "momo:webview";

// 1) 映射到打包资源目录(assets/web/): 原生直接读文件应答, 不经过 JS。
registerProtocol("app", { assets: "web", fallback: "index.html" });

// 2) JS 处理函数: 动态内容、本地 API。可以异步。
registerProtocol("api", async (request) => ({
  mimeType: "application/json",
  headers: { "Access-Control-Allow-Origin": "*" },
  body: JSON.stringify({ path: new URL(request.url).pathname }),
}));

new WebView({ initialUrl: "app://localhost/" });
```

- **必须在创建 WebView 之前声明**(通常在应用启动时)。协议集合在 WebView 创建时确定。
- 默认每个 WebView 都能用全部已声明的协议。页面可以向它可用的任何协议发请求, 所以加载不受信任网站的 WebView
  要收窄: `new WebView({ protocols: ["app"] })`(`[]` 为一个都不给)。
- 注册协议与打开网页是两回事: WebView 不校验协议。地址用了没有注册(或没给这个视图)的协议, 就是一次失败的导航,
  经 `web.onError` 报告。
- 这类地址是真实地址: 刷新、前进后退、相对路径、`fetch` 都有效。
- 源由 协议 + host 决定: `app://localhost` 请求 `api://host` 是跨源, 响应要带 CORS 头。
- 资源映射: 目录地址取 `index`(默认 `index.html`);找不到文件返回 404, 或改用 `fallback`(单页应用的前端路由);
  路径不能越出资源目录。
- 资源文件在后台线程读取, 不阻塞界面。
- 自定义协议的页面没有 cookie(`document.cookie` 无效);用 `localStorage` 或由 JS 处理函数管理状态。
- 目前整个响应一次给出, 不支持 Range 请求(大的音视频不适合走这里)。

## 会话与 cookie

会话是一份相互隔离的网站数据(cookie、缓存、localStorage / IndexedDB…)。WebView 的会话在创建时确定。

```ts
import { WebSession, WebView } from "momo:webview";

WebSession.default;               // 默认, 持久化
WebSession.persistent("work");    // 命名的持久会话(多账号);同名即同一份数据。macOS 14+
const incognito = WebSession.ephemeral();   // 无痕, 只在内存;用完 dispose()

// cookie 不依赖 WebView: 可以先预置登录态再创建视图。
await incognito.cookies.set({ name: "sid", value: "42", url: "https://example.com/", secure: true, httpOnly: true });
await incognito.cookies.get("https://example.com/");   // 会随该地址发送的 cookie;不给 url 则全部
await incognito.cookies.delete("sid", { domain: "example.com" });
await incognito.cookies.clear();
await WebSession.default.clearData(["cache"]);          // "cookies" | "cache" | "storage";不给则全部

new WebView({ session: incognito, initialUrl: "https://example.com/" });
```

`expires` 是 Unix 秒, `null` 为会话 cookie。WebKit 会把过期时间截到最多约 400 天之后。

## WebView

```ts
new WebView({
  initialUrl: "app://localhost/",
  initScripts: ["window.__env = 'prod';"],   // 每个文档开始时、页面脚本之前(主框架)
  userAgent: "MyApp/1.0",
  devtools: true,                            // 允许系统网页检查器
  onCreated: (web) => {
    web.onStateChanged = (s) => console.log(s.url, s.title, s.loading, s.progress);
    web.onMessage = (data, source) => {                   // 页面: window.momo.postMessage(value)
      if (source.origin === "app://localhost") console.log(data);
    };
    web.onNewWindowRequested = (url) => web.loadUrl(url); // target=_blank / window.open
    web.onError = (message) => console.error(message);
  },
});
```

`WebViewController`: `loadUrl / loadHtml / goBack / goForward / reload / stop / evaluateJavaScript /
postMessage / focus`, 以及上面的回调。

页面与应用互发消息(值经 JSON 传递), 两个平台写法相同。`window.momo` 存在于这个 WebView 加载的**每一个**页面里,
包括跳转到的外部网站: `onMessage` 的第二个参数给出发送方的源, 处理前先核对。

```js
window.momo.postMessage({ type: "ready" });                       // 页面 → 应用(onMessage)
window.momo.addEventListener("message", (e) => console.log(e.data)); // 应用 → 页面(web.postMessage)
```

## 平台映射

| 接口 | WKWebView(已实现) | WebView2(设计依据, 未实现) |
| --- | --- | --- |
| `registerProtocol` | `WKWebViewConfiguration.setURLSchemeHandler`(创建视图前) | `CoreWebView2CustomSchemeRegistration`(创建环境前)+ `WebResourceRequested` |
| `WebSession.default` | `WKWebsiteDataStore.default()` | 默认 profile |
| `WebSession.persistent(name)` | `WKWebsiteDataStore(forIdentifier:)`(macOS 14+) | `ControllerOptions.ProfileName` |
| `WebSession.ephemeral()` | `.nonPersistent()` | `ControllerOptions.IsInPrivateModeEnabled` |
| `cookies.*` | `WKHTTPCookieStore` | `CoreWebView2CookieManager` |
| `clearData` | `removeData(ofTypes:modifiedSince:)` | `Profile.ClearBrowsingDataAsync` |
| `initScripts` | `WKUserScript`(atDocumentStart) | `AddScriptToExecuteOnDocumentCreated` |
| `window.momo` 消息 | `WKScriptMessageHandler` + 注入脚本 | `chrome.webview.postMessage` / `WebMessageReceived` + 注入脚本 |
| `userAgent` | `customUserAgent` | `Settings.UserAgent` |
| `devtools` | `isInspectable` | `Settings.AreDevToolsEnabled` |
| `onNewWindowRequested` | `WKUIDelegate` createWebView 返回 nil | `NewWindowRequested`(`Handled = true`) |

## 生命周期

WebView 的原生视图与它在视图树里的 `State` 同生共死: 组件移出树即销毁(之后对 controller 的调用报错), 页面状态不保留。
给 `WebView` 一个 `GlobalKey`, 就可以把它移到视图树的别处——包括另一个窗口——而不销毁: 原生视图换窗口, 页面不刷新,
`viewId`、controller、消息与协议请求都照常工作(示例浏览器的"在新窗口中打开"就是这样做的)。

"协议在创建 WebView 之前声明"这条规则来自两边的共同约束: WebView2 要求在创建环境之前登记协议,
WKWebView 要求在创建视图之前。导航拦截(允许/拒绝某次跳转)没有纳入: WebView2 的 `NavigationStarting`
不能异步等待 JS 的决定, 需要另行设计成声明式规则。

## 示例与构建

示例应用 `examples/webview` 是一个多窗口、多标签浏览器(标签可以连同网页移到新窗口而不重新加载), 其起始页
`momo://newtab` 就是用 `registerProtocol` 声明的。运行(macOS;`momo:webview` 已在构建出的运行时里, 示例不需要 `npm install`):

```bash
(cd examples/webview && npm run build) && out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev examples/webview
node framework/packages/cli/momo.mjs build examples/webview --runtime=out/debug/embedder/Momo.app   # 打包成 examples/webview/dist/MomoBrowser.app
```

只实现了 macOS;Windows 需要 embedder 侧的平台视图支持(契约已就绪, Windows 宿主未实现)。
