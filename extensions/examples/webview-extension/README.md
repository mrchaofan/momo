# @momo-examples/ext-webview

系统 WebView(macOS `WKWebView`)作为**平台视图**嵌入 momo 界面的示例扩展。原理见 `docs/05-rendering-pipeline.md` §9,
扩展侧接口见 `docs/07-extension-system.md` §3。

- 原生部分 `native/src/ext_webview.mm`:只用 C ABI(`momo_extension.hpp`),注册视图类型 `momo.webview`,
  处理命令通道 `momo.webview`,经 `momo.webview/events` 推送状态与页面消息。协议在文件头。
- JS 门面 `js/index.ts`:`WebView` view 与 `WebViewController`(loadUrl / loadHtml / goBack / goForward /
  reload / stop / evaluateJavaScript / focus,以及 onStateChanged / onMessage / onError 回调)。

```ts
import { WebView } from "@momo-examples/ext-webview";

new SizedBox({ height: 400, child: new WebView({
  initialUrl: "https://example.com",
  onCreated: (web) => { web.onStateChanged = (s) => console.log(s.title); },
}) });
```

页面向 momo 发消息:`window.webkit.messageHandlers.momo.postMessage("hi")`。

示例应用 `examples/webview` 是一个多窗口、多标签浏览器(标签可以连同网页移到新窗口而不重新加载)。
运行(macOS,需先构建 `momo_app_mac` 与 `ext_webview`):

```bash
node tools/momo-cli/momo.mjs build examples/webview
```

只实现了 macOS;Windows 需要 embedder 侧的平台视图支持(契约已就绪, Windows 宿主未实现)。
