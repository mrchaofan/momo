#pragma once

// momo.webview 的平台契约: 插件的平台无关部分(webview_extension.cc: Node-API、命令协议、协议帧、cookie 的筛选)
// 与系统 WebView 之间的全部交互。一个平台 = 实现 WebViewPlatform 与 WebView, 再定义 WebViewPlatform::Create:
//   macOS   webview_mac.mm   WKWebView(平台视图为 NSView*)
//   Windows webview_win.cc   WebView2(平台视图为 MomoWinPlatformView*, 经 DirectComposition 合成)
// 平台实现不认识 JS、JSON 线上格式与请求号;这里的类型就是两边之间的全部词汇。
//
// 线程: 全部方法与回调都在 platform 线程(宿主的视图工厂、post_platform_task 的任务、系统 WebView 的回调)。
// Reply / CookiesReply 恰好调用一次(可以异步, 仍在 platform 线程)。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "json.h"

namespace momo_webview {

// 创建参数(PlatformView.creationParams, 线上格式见 webview_extension.cc 文件头)。
struct WebViewParams {
  std::optional<std::string> url;
  std::optional<std::string> html;
  std::string base_url;
  std::string session = "default";  // 会话键: "default" | "persistent:<name>" | "ephemeral:<id>"
  std::string user_agent;            // 空 = 系统默认
  bool devtools = false;
  std::vector<std::string> init_scripts;
  std::vector<std::string> protocols;  // 本视图可用的自定义协议
};

struct WebViewState {
  std::string url;
  std::string title;
  bool loading = false;
  double progress = 0;
  bool can_go_back = false;
  bool can_go_forward = false;
};

struct Cookie {
  std::string name;
  std::string value;
  std::string domain;  // 以 "." 开头表示对子域也有效
  std::string path = "/";
  std::optional<double> expires;  // Unix 秒;空 = 会话 cookie
  bool secure = false;
  bool http_only = false;
  std::string same_site;  // "" | "lax" | "strict"
};

using Headers = std::map<std::string, std::string>;

// 页面对自定义协议地址的一次请求与它的应答。
struct ProtocolRequest {
  std::string url;
  std::string method = "GET";
  Headers headers;
  std::string body;
};
struct ProtocolResponse {
  int status = 200;
  Headers headers;
  std::string body;
};

// 异步操作的结果: error 为空表示成功, value 为结果(可 JSON 化的值)。
struct Outcome {
  Json value;
  std::string error;
  static Outcome Ok(Json value = {}) { return {std::move(value), {}}; }
  static Outcome Fail(std::string error) { return {Json(), std::move(error)}; }
};
using Reply = std::function<void(Outcome)>;
using CookiesReply = std::function<void(std::vector<Cookie> cookies, std::string error)>;
// 自定义协议的应答回调(恰好调用一次, 平台已取消的请求不会再被应答)。
using Respond = std::function<void(const ProtocolResponse&)>;

// 平台 → 插件: WebView 里发生的事。
class WebViewHost {
 public:
  virtual void OnStateChanged(int64_t view, const WebViewState& state) = 0;
  // 页面里 window.momo.postMessage(value): body 为 JSON 文本, origin 为发送方的源(不透明源为 "null")。
  virtual void OnMessage(int64_t view, const std::string& body, const std::string& origin) = 0;
  // 导航失败(含: 地址的协议不受支持, 也没有注册给这个视图)。
  virtual void OnNavigationError(int64_t view, const std::string& message) = 0;
  // 页面请求新窗口(target=_blank / window.open);平台不创建。
  virtual void OnNewWindow(int64_t view, const std::string& url) = 0;
  // 自定义协议的请求: 转给 JS 的处理函数, 应答到达时调用 respond。返回请求号, 平台取消请求(导航取消、视图销毁)时
  // 以它调用 CancelProtocolRequest, 之后 respond 不会被调用。
  virtual uint64_t OnProtocolRequest(int64_t view, ProtocolRequest request, Respond respond) = 0;
  virtual void CancelProtocolRequest(uint64_t request) = 0;
  virtual void Log(const std::string& message) = 0;

 protected:
  ~WebViewHost() = default;
};

// 一个系统 WebView。销毁 = 视图被 dispose(宿主已把它移出窗口)。
class WebView {
 public:
  virtual ~WebView() = default;
  // 交给宿主的平台视图(momo_host.h 的 create 的返回值);在本对象存续期间有效。
  virtual void* native_view() = 0;
  // url 无法解析时返回 false。
  virtual bool LoadUrl(const std::string& url) = 0;
  virtual void LoadHtml(const std::string& html, const std::string& base_url) = 0;
  virtual void GoBack() = 0;
  virtual void GoForward() = 0;
  virtual void Reload() = 0;
  virtual void Stop() = 0;
  // 结果为脚本的值(可 JSON 化的部分;其余转为字符串)。
  virtual void EvaluateJavaScript(const std::string& script, Reply reply) = 0;
  // 页内查找: 从当前选区继续, 到头回绕;结果为是否找到(bool)。
  virtual void Find(const std::string& text, bool backwards, bool case_sensitive, Reply reply) = 0;
};

// 系统 WebView 的工厂与会话(数据分区)。进程内一个。
class WebViewPlatform {
 public:
  // 每个平台的实现文件各定义一次。
  static std::unique_ptr<WebViewPlatform> Create(WebViewHost& host);

  virtual ~WebViewPlatform() = default;
  // 应用声明了一个自定义协议(JS 的 registerProtocol, 先于用到它的 WebView)。
  virtual void DeclareScheme(const std::string& scheme) = 0;
  // 失败时返回空, 并给出原因。
  virtual std::unique_ptr<WebView> CreateView(int64_t id, const WebViewParams& params, std::string* error) = 0;

  // ---- 会话(session 为会话键)。会话不依赖 WebView: 没有 WebView 时也能读写 cookie ----
  virtual void GetCookies(const std::string& session, CookiesReply reply) = 0;
  virtual void SetCookie(const std::string& session, const Cookie& cookie, Reply reply) = 0;
  virtual void DeleteCookie(const std::string& session, const Cookie& cookie, Reply reply) = 0;
  // kinds: "cookies" | "cache" | "storage";空 = 全部类别。
  virtual void ClearData(const std::string& session, const std::vector<std::string>& kinds, Reply reply) = 0;
  // 释放无痕会话(对持久会话无效果)。
  virtual void DisposeSession(const std::string& session) = 0;
};

// 页面侧的消息桥: 定义 window.momo(EventTarget), postMessage(value) 以 JSON 文本交给 post(由平台给出, 如
// "window.webkit.messageHandlers.momo.postMessage")。只在主框架定义。
std::string BridgeScript(const std::string& post);

}  // namespace momo_webview
