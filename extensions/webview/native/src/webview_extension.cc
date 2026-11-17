// ext_webview: 系统 WebView 作为平台视图嵌入 momo(ARCHITECTURE.md §4.10、docs/07 §3)。本文件是插件的平台无关部分:
// Node-API 导出、JS 与 platform 线程之间的投递、命令协议、自定义协议的帧与在途请求、cookie 的筛选。系统 WebView
// 在平台实现里(webview_platform.h: macOS WKWebView、Windows WebView2)。插件不链接引擎任何符号: 与引擎之间只用
// 宿主能力表(momo_host.h)——注册平台视图类型、把任务投到 platform 线程。接口设计见本扩展的 README。
//
// 视图类型 "momo.webview"。
//
// 创建参数(PlatformView.creationParams, JSON):
//   {"url"?, "html"?, "baseUrl"?, "session"?: <会话键>, "userAgent"?, "devtools"?: bool,
//    "initScripts"?: [string], "protocols"?: [{"scheme"}]}
//   protocols: 该 WebView 可用的自定义资源协议;请求都转给 JS 的处理函数。
//   会话键: "default" | "persistent:<name>" | "ephemeral:<id>"。
//
// 插件导出(JS 门面 js/index.ts 是唯一的调用方):
//   attach(nativeHost, {onEvent, onProtocolRequest})   只调用一次
//   command(json) → Promise<json>          回复 {"result": <any>} 或 {"error": <string>}
//   respondToProtocolRequest(frame)        JS 协议处理函数的应答
//
// 命令(JS → 原生, JSON):
//   {"method": "declareProtocol", "scheme"}   应用声明了一个自定义协议(registerProtocol;WebView2 在启动时一次性
//       登记全部协议, 所以要先于 WebView 与会话操作知道它们)
//   视图命令 {"id": <viewId>, "method", ...}:
//       loadUrl {url} | loadHtml {html, baseUrl?} | goBack | goForward | reload | stop |
//       evaluateJavaScript {script} |
//       find {text, backwards?, caseSensitive?} → bool   页内查找: 选中并滚动到下一处(或上一处)匹配, 到头回绕
//   会话命令 {"session": <会话键>, "method", ...}:
//       cookies.get {url?} → [cookie] | cookies.set {cookie} | cookies.delete {name, url?, domain?, path?} → 个数 |
//       session.clearData {kinds?: ["cookies"|"cache"|"storage"]} | session.dispose
//   cookie: {"name", "value", "domain", "path", "expires": <Unix 秒>|null, "secure", "httpOnly",
//            "sameSite": "lax"|"strict"|null};cookies.set 可用 "url" 代替 "domain"。
//
// 事件 onEvent(json)(原生 → JS):
//   {"id", "event": "state", "url", "title", "loading", "progress", "canGoBack", "canGoForward"}
//   {"id", "event": "message", "body", "origin"}   页面里 window.momo.postMessage(value);body 为 JSON 文本,
//                                                  origin 为发送方的源("app://localhost";不透明源为 "null")
//   {"id", "event": "error", "message"}   导航失败(含: 地址的协议不受支持, 也没有注册给这个视图)
//   {"id", "event": "newWindow", "url"}   页面请求新窗口(target=_blank / window.open);原生不创建
//
// 自定义协议(二进制帧: uint32 LE 头长度 + 头 JSON + 正文字节):
//   onProtocolRequest(frame)        头 {"requestId", "viewId", "url", "method", "headers"}, 正文为请求体
//   respondToProtocolRequest(frame) 头 {"requestId", "status", "headers"}, 正文为响应体(处理函数抛错时 JS 侧应答 500)
//
// 线程: 导出的函数与两个 JS 回调在 JS 线程;视图工厂、命令的执行、系统 WebView 的回调都在 platform(主)线程。
//   JS → platform: post_platform_task;platform → JS: 一个 napi_threadsafe_function。

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <node_api.h>

#include "json.h"
#include "momo/momo_host.h"
#include "webview_platform.h"

namespace momo_webview {

namespace {

constexpr char kViewType[] = "momo.webview";

// ---- 协议帧: uint32 LE 头长度 + 头 JSON + 正文 ----

std::string EncodeFrame(const Json& header, const std::string& body) {
  const std::string json = header.Dump();
  const auto length = static_cast<uint32_t>(json.size());
  std::string frame;
  frame.reserve(4 + json.size() + body.size());
  for (int shift = 0; shift < 32; shift += 8) frame += static_cast<char>((length >> shift) & 0xFF);
  frame += json;
  frame += body;
  return frame;
}

bool DecodeFrame(const std::string& frame, Json* header, std::string* body) {
  if (frame.size() < 4) return false;
  size_t length = 0;
  for (int i = 0; i < 4; ++i) length |= static_cast<size_t>(static_cast<unsigned char>(frame[i])) << (8 * i);
  if (length > frame.size() - 4) return false;
  std::optional<Json> parsed = Json::Parse(std::string_view(frame).substr(4, length));
  if (!parsed) return false;
  *header = std::move(*parsed);
  *body = frame.substr(4 + length);
  return true;
}

Headers HeadersFrom(const Json& json) {
  Headers headers;
  if (const Json::Object* object = json.object()) {
    for (const auto& [name, value] : *object) {
      if (const std::string* text = value.string()) headers[name] = *text;
    }
  }
  return headers;
}

Json HeadersToJson(const Headers& headers) {
  Json::Object object;
  for (const auto& [name, value] : headers) object[name] = value;
  return object;
}

// ---- cookie: 线上格式与"会随哪个地址发送"的判断(平台只提供读、写、删) ----

std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// 只取判断 cookie 需要的部分: scheme://[userinfo@]host[:port]/path。
struct Url {
  std::string scheme;
  std::string host;
  std::string path = "/";

  static std::optional<Url> Parse(const std::string& text) {
    const size_t colon = text.find("://");
    if (colon == std::string::npos || colon == 0) return std::nullopt;
    Url url;
    url.scheme = Lower(text.substr(0, colon));
    const size_t authority_start = colon + 3;
    const size_t authority_end = text.find_first_of("/?#", authority_start);
    std::string authority = text.substr(authority_start, authority_end == std::string::npos
                                                            ? std::string::npos
                                                            : authority_end - authority_start);
    if (const size_t at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    if (const size_t port = authority.rfind(':');
        port != std::string::npos && authority.find(']') == std::string::npos) {
      authority = authority.substr(0, port);
    }
    url.host = Lower(authority);
    if (url.host.empty()) return std::nullopt;
    if (authority_end != std::string::npos && text[authority_end] == '/') {
      const size_t path_end = text.find_first_of("?#", authority_end);
      url.path = text.substr(authority_end, path_end == std::string::npos ? std::string::npos
                                                                             : path_end - authority_end);
    }
    return url;
  }
};

bool DomainMatches(const std::string& cookie_domain, const std::string& host) {
  std::string domain = Lower(cookie_domain);
  if (!domain.empty() && domain[0] == '.') domain.erase(0, 1);
  const std::string h = Lower(host);
  return h == domain || (h.size() > domain.size() && h.compare(h.size() - domain.size(), domain.size(), domain) == 0 &&
                         h[h.size() - domain.size() - 1] == '.');
}

bool PathMatches(const std::string& cookie_path, const std::string& path) {
  if (cookie_path.empty() || cookie_path == "/") return true;
  const std::string p = path.empty() ? "/" : path;
  if (p == cookie_path) return true;
  if (p.compare(0, cookie_path.size(), cookie_path) != 0) return false;
  return cookie_path.back() == '/' || p[cookie_path.size()] == '/';
}

// 该 cookie 是否会随对 url 的请求发送。
bool CookieAppliesTo(const Cookie& cookie, const Url& url) {
  if (!DomainMatches(cookie.domain, url.host)) return false;
  if (cookie.secure && url.scheme != "https") return false;
  return PathMatches(cookie.path, url.path);
}

Json CookieToJson(const Cookie& cookie) {
  Json::Object o;
  o["name"] = cookie.name;
  o["value"] = cookie.value;
  o["domain"] = cookie.domain;
  o["path"] = cookie.path.empty() ? "/" : cookie.path;
  o["expires"] = cookie.expires ? Json(*cookie.expires) : Json();
  o["secure"] = cookie.secure;
  o["httpOnly"] = cookie.http_only;
  o["sameSite"] = cookie.same_site.empty() ? Json() : Json(cookie.same_site);
  return o;
}

// domain 与 url 至少给一个: 只给 url 时为该主机的 host-only cookie。
std::optional<Cookie> CookieFromJson(const Json& json) {
  Cookie cookie;
  cookie.name = json.String("name");
  cookie.value = json.String("value");
  cookie.domain = json.String("domain");
  if (cookie.domain.empty()) {
    if (std::optional<Url> url = Url::Parse(json.String("url"))) cookie.domain = url->host;
  }
  if (cookie.name.empty() || cookie.domain.empty()) return std::nullopt;
  cookie.path = json.String("path", "/");
  cookie.expires = json.Number("expires");
  cookie.secure = json.Bool("secure");
  cookie.http_only = json.Bool("httpOnly");
  const std::string same_site = json.String("sameSite");
  if (same_site == "lax" || same_site == "strict") cookie.same_site = same_site;
  return cookie;
}

WebViewParams ParamsFromJson(const Json& json) {
  WebViewParams params;
  params.url = json.OptionalString("url");
  params.html = json.OptionalString("html");
  params.base_url = json.String("baseUrl");
  params.session = json.String("session", "default");
  params.user_agent = json.String("userAgent");
  params.devtools = json.Bool("devtools");
  params.init_scripts = json.Strings("initScripts");
  if (const Json::Array* protocols = json["protocols"].array()) {
    for (const Json& spec : *protocols) {
      const std::string scheme = spec.String("scheme");
      if (!scheme.empty()) params.protocols.push_back(scheme);
    }
  }
  return params;
}

// 发往 JS 线程的一条消息。
struct JsMessage {
  enum Kind { kReply, kEvent, kProtocolRequest };
  Kind kind;
  uint64_t token;  // kReply: command() 返回的 Promise
  std::string data;
};

}  // namespace

std::string BridgeScript(const std::string& post) {
  return "(function(){if(window!==window.top||window.momo)return;var t=new EventTarget();"
         "t.postMessage=function(v){" + post + "(JSON.stringify(v===undefined?null:v));};"
         "Object.defineProperty(window,'momo',{value:t});})();";
}

// 进程里只有一个(模块初始化时创建), 不释放: platform 线程上的状态(系统 WebView、在途的回调)比 JS 运行时活得久。
class WebViewExtension final : public WebViewHost {
 public:
  // ---- JS 线程: 插件导出的函数 ----

  // attach(nativeHost, {onEvent, onProtocolRequest})
  napi_value Attach(napi_env env, size_t argc, napi_value* argv) {
    const MomoHost* host = argc < 2 ? nullptr : momo_host_from_napi(env, argv[0]);
    napi_value on_event, on_protocol_request, name;
    if (host == nullptr || host_ != nullptr ||
        napi_get_named_property(env, argv[1], "onEvent", &on_event) != napi_ok ||
        napi_create_reference(env, on_event, 1, &on_event_) != napi_ok ||
        napi_get_named_property(env, argv[1], "onProtocolRequest", &on_protocol_request) != napi_ok ||
        napi_create_reference(env, on_protocol_request, 1, &on_protocol_request_) != napi_ok) {
      napi_throw_type_error(env, nullptr, "attach(nativeHost, {onEvent, onProtocolRequest}) may be called once");
      return nullptr;
    }
    napi_create_string_utf8(env, kViewType, NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, nullptr, nullptr, name, 0, 1, nullptr, nullptr, this, &DeliverToJs,
                                    &to_js_);
    napi_add_env_cleanup_hook(env, &OnRuntimeDestroyed, this);
    const MomoPlatformViewFactory factory = {sizeof(MomoPlatformViewFactory), &CreateView, &DisposeView, this};
    if (host->platform_view_register_factory(host, kViewType, &factory) != kMomoOk) {
      napi_throw_error(env, nullptr, "cannot register the momo.webview platform view type");
      return nullptr;
    }
    host_ = host;  // 此后只读;platform 线程经由下面投递的任务才看到它
    return nullptr;
  }

  // command(json) → Promise<json>
  napi_value Command(napi_env env, size_t argc, napi_value* argv) {
    std::string json;
    if (argc < 1 || !StringFrom(env, argv[0], &json)) {
      napi_throw_type_error(env, nullptr, "command(json) expects a string");
      return nullptr;
    }
    napi_deferred deferred;
    napi_value promise;
    napi_create_promise(env, &deferred, &promise);
    const uint64_t token = ++next_token_;
    if (PostToPlatform([this, json = std::move(json), token] { HandleCommand(json, token); })) {
      deferreds_[token] = deferred;
    } else {
      napi_value message, error;
      napi_create_string_utf8(env, "the engine is shutting down", NAPI_AUTO_LENGTH, &message);
      napi_create_error(env, nullptr, message, &error);
      napi_reject_deferred(env, deferred, error);
    }
    return promise;
  }

  // respondToProtocolRequest(frame: ArrayBuffer)
  napi_value RespondToProtocolRequest(napi_env env, size_t argc, napi_value* argv) {
    void* bytes = nullptr;
    size_t size = 0;
    if (argc < 1 || napi_get_arraybuffer_info(env, argv[0], &bytes, &size) != napi_ok) {
      napi_throw_type_error(env, nullptr, "respondToProtocolRequest(frame) expects an ArrayBuffer");
      return nullptr;
    }
    std::string frame(static_cast<const char*>(bytes), size);
    PostToPlatform([this, frame = std::move(frame)] { HandleProtocolResponse(frame); });
    return nullptr;
  }

  // ---- WebViewHost(platform 线程) ----

  void OnStateChanged(int64_t view, const WebViewState& state) override {
    SendEvent(Json::Object{{"id", view},
                           {"event", "state"},
                           {"url", state.url},
                           {"title", state.title},
                           {"loading", state.loading},
                           {"progress", state.progress},
                           {"canGoBack", state.can_go_back},
                           {"canGoForward", state.can_go_forward}});
  }

  void OnMessage(int64_t view, const std::string& body, const std::string& origin) override {
    SendEvent(Json::Object{{"id", view}, {"event", "message"}, {"body", body}, {"origin", origin}});
  }

  void OnNavigationError(int64_t view, const std::string& message) override {
    SendEvent(Json::Object{{"id", view}, {"event", "error"}, {"message", message}});
  }

  void OnNewWindow(int64_t view, const std::string& url) override {
    SendEvent(Json::Object{{"id", view}, {"event", "newWindow"}, {"url", url}});
  }

  uint64_t OnProtocolRequest(int64_t view, ProtocolRequest request, Respond respond) override {
    const uint64_t id = ++next_request_id_;
    pending_[id] = std::move(respond);
    const Json header = Json::Object{{"requestId", static_cast<int64_t>(id)},
                                     {"viewId", view},
                                     {"url", request.url},
                                     {"method", request.method},
                                     {"headers", HeadersToJson(request.headers)}};
    PostToJs(JsMessage::kProtocolRequest, 0, EncodeFrame(header, request.body));
    return id;
  }

  void CancelProtocolRequest(uint64_t request) override { pending_.erase(request); }

  void Log(const std::string& message) override { std::fprintf(stderr, "[momo.webview] %s\n", message.c_str()); }

 private:
  // ---- 两个方向的投递 ----

  // JS 线程 → platform 线程。引擎已关闭时返回 false。(关闭前投递、关闭时还没执行的任务被宿主丢弃。)
  bool PostToPlatform(std::function<void()> task) {
    if (host_ == nullptr) return false;
    auto* heap = new std::function<void()>(std::move(task));
    if (host_->post_platform_task(host_, &RunTask, heap) == kMomoOk) return true;
    delete heap;
    return false;
  }
  static void RunTask(void* task) {
    std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(task));
    (*owned)();
  }

  // platform 线程 → JS 线程。JS 运行时已销毁时丢弃。
  void PostToJs(JsMessage::Kind kind, uint64_t token, std::string data) {
    std::lock_guard<std::mutex> lock(to_js_mutex_);
    if (to_js_ == nullptr) return;
    auto* message = new JsMessage{kind, token, std::move(data)};
    if (napi_call_threadsafe_function(to_js_, message, napi_tsfn_nonblocking) != napi_ok) delete message;
  }

  void SendEvent(const Json& event) { PostToJs(JsMessage::kEvent, 0, event.Dump()); }

  // JS 线程: 运行时销毁时的清理函数。此前宿主已关闭宿主表并 dispose 全部视图(momo_host.h 的"关闭"),
  // 所以只剩一件事: 不再向 JS 投递(系统 WebView 的在途回调可能更晚才到)。
  static void OnRuntimeDestroyed(void* self) {
    auto* extension = static_cast<WebViewExtension*>(self);
    napi_threadsafe_function to_js;
    {
      std::lock_guard<std::mutex> lock(extension->to_js_mutex_);
      to_js = extension->to_js_;
      extension->to_js_ = nullptr;
    }
    napi_release_threadsafe_function(to_js, napi_tsfn_abort);
  }

  // JS 线程: 一条消息到达。env 为空表示运行时正在销毁(只释放消息)。
  static void DeliverToJs(napi_env env, napi_value, void* self, void* data) {
    std::unique_ptr<JsMessage> message(static_cast<JsMessage*>(data));
    if (env != nullptr) static_cast<WebViewExtension*>(self)->Deliver(env, *message);
  }
  void Deliver(napi_env env, const JsMessage& message) {
    napi_value value;
    if (message.kind == JsMessage::kProtocolRequest) {
      void* bytes = nullptr;
      napi_create_arraybuffer(env, message.data.size(), &bytes, &value);
      if (!message.data.empty()) std::memcpy(bytes, message.data.data(), message.data.size());
      CallJs(env, on_protocol_request_, value);
      return;
    }
    napi_create_string_utf8(env, message.data.data(), message.data.size(), &value);
    if (message.kind == JsMessage::kEvent) {
      CallJs(env, on_event_, value);
      return;
    }
    auto it = deferreds_.find(message.token);
    if (it != deferreds_.end()) {
      napi_resolve_deferred(env, it->second, value);
      deferreds_.erase(it);
    }
  }
  // 调用 JS 回调;它抛出的异常按未捕获异常上报。
  static void CallJs(napi_env env, napi_ref callback, napi_value argument) {
    napi_value function, receiver, error;
    napi_get_reference_value(env, callback, &function);
    napi_get_undefined(env, &receiver);
    if (napi_call_function(env, receiver, function, 1, &argument, nullptr) == napi_pending_exception &&
        napi_get_and_clear_last_exception(env, &error) == napi_ok) {
      napi_fatal_exception(env, error);
    }
  }
  static bool StringFrom(napi_env env, napi_value value, std::string* out) {
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return false;
    out->resize(length + 1);
    napi_get_value_string_utf8(env, value, out->data(), length + 1, &length);
    out->resize(length);
    return true;
  }

  // ---- 视图工厂(platform 线程, 宿主调用) ----

  static void* CreateView(void* self, int64_t view_id, const uint8_t* bytes, size_t size) {
    return static_cast<WebViewExtension*>(self)->Create(view_id, bytes, size);
  }
  static void DisposeView(void* self, int64_t view_id, void*) {
    static_cast<WebViewExtension*>(self)->views_.erase(view_id);
  }

  WebViewPlatform& platform() {
    if (platform_ == nullptr) platform_ = WebViewPlatform::Create(*this);
    return *platform_;
  }

  void* Create(int64_t view_id, const uint8_t* bytes, size_t size) {
    const std::optional<Json> json =
        Json::Parse(std::string_view(reinterpret_cast<const char*>(bytes), bytes != nullptr ? size : 0));
    std::string error;
    std::unique_ptr<WebView> view =
        platform().CreateView(view_id, ParamsFromJson(json.value_or(Json::Object{})), &error);
    if (view == nullptr) {
      Log("cannot create the WebView: " + error);
      return nullptr;
    }
    void* native = view->native_view();
    views_[view_id] = std::move(view);
    return native;
  }

  // ---- 命令(platform 线程) ----

  void Reply(uint64_t token, Outcome outcome) {
    const Json payload = outcome.error.empty() ? Json(Json::Object{{"result", std::move(outcome.value)}})
                                               : Json(Json::Object{{"error", std::move(outcome.error)}});
    PostToJs(JsMessage::kReply, token, payload.Dump());
  }
  momo_webview::Reply ReplyTo(uint64_t token) {
    return [this, token](Outcome outcome) { Reply(token, std::move(outcome)); };
  }

  void HandleCommand(const std::string& text, uint64_t token) {
    const Json call = Json::Parse(text).value_or(Json::Object{});
    const std::string method = call.String("method");
    if (method.empty()) return Reply(token, Outcome::Fail("missing method"));
    if (method == "declareProtocol") {
      platform().DeclareScheme(call.String("scheme"));
      return Reply(token, Outcome::Ok());
    }
    if (method.rfind("cookies.", 0) == 0 || method.rfind("session.", 0) == 0) {
      return HandleSessionCommand(call, method, token);
    }
    const std::optional<double> id = call.Number("id");
    auto it = id ? views_.find(static_cast<int64_t>(*id)) : views_.end();
    if (it == views_.end()) return Reply(token, Outcome::Fail("unknown view id"));
    WebView& web = *it->second;
    if (method == "loadUrl") {
      if (!web.LoadUrl(call.String("url"))) return Reply(token, Outcome::Fail("invalid url"));
    } else if (method == "loadHtml") {
      web.LoadHtml(call.String("html"), call.String("baseUrl"));
    } else if (method == "goBack") {
      web.GoBack();
    } else if (method == "goForward") {
      web.GoForward();
    } else if (method == "reload") {
      web.Reload();
    } else if (method == "stop") {
      web.Stop();
    } else if (method == "evaluateJavaScript") {
      return web.EvaluateJavaScript(call.String("script"), ReplyTo(token));
    } else if (method == "find") {
      return web.Find(call.String("text"), call.Bool("backwards"), call.Bool("caseSensitive"), ReplyTo(token));
    } else {
      return Reply(token, Outcome::Fail("unknown method " + method));
    }
    Reply(token, Outcome::Ok());
  }

  void HandleSessionCommand(const Json& call, const std::string& method, uint64_t token) {
    const std::string session = call.String("session", "default");
    if (method == "session.dispose") {
      platform().DisposeSession(session);
      Reply(token, Outcome::Ok());
    } else if (method == "session.clearData") {
      const Json& kinds = call["kinds"];
      std::vector<std::string> list = call.Strings("kinds");
      if (kinds.array() != nullptr && list.empty()) return Reply(token, Outcome::Ok());  // 空列表: 什么也不清除
      platform().ClearData(session, list, ReplyTo(token));
    } else if (method == "cookies.get") {
      std::optional<Url> url;
      if (std::optional<std::string> text = call.OptionalString("url")) {
        url = Url::Parse(*text);
        if (!url) return Reply(token, Outcome::Fail("invalid url"));
      }
      platform().GetCookies(session, [this, token, url](std::vector<Cookie> cookies, std::string error) {
        if (!error.empty()) return Reply(token, Outcome::Fail(std::move(error)));
        Json::Array result;
        for (const Cookie& cookie : cookies) {
          if (!url || CookieAppliesTo(cookie, *url)) result.push_back(CookieToJson(cookie));
        }
        Reply(token, Outcome::Ok(std::move(result)));
      });
    } else if (method == "cookies.set") {
      std::optional<Cookie> cookie = CookieFromJson(call["cookie"]);
      if (!cookie) return Reply(token, Outcome::Fail("invalid cookie (name and domain or url are required)"));
      platform().SetCookie(session, *cookie, ReplyTo(token));
    } else if (method == "cookies.delete") {
      DeleteCookies(session, call, token);
    } else {
      Reply(token, Outcome::Fail("unknown method " + method));
    }
  }

  // cookies.delete: 删除名字匹配、且在给定范围(url / domain / path)内的 cookie, 应答删除的个数。
  void DeleteCookies(const std::string& session, const Json& call, uint64_t token) {
    const std::string name = call.String("name");
    const std::optional<std::string> domain = call.OptionalString("domain");
    const std::optional<std::string> path = call.OptionalString("path");
    std::optional<Url> url;
    if (std::optional<std::string> text = call.OptionalString("url")) url = Url::Parse(*text);
    platform().GetCookies(session, [=](std::vector<Cookie> all, std::string error) {
      if (!error.empty()) return Reply(token, Outcome::Fail(std::move(error)));
      auto doomed = std::make_shared<std::vector<Cookie>>();
      for (Cookie& cookie : all) {
        if (cookie.name != name) continue;
        if (url && !CookieAppliesTo(cookie, *url)) continue;
        if (domain && !DomainMatches(cookie.domain, *domain)) continue;
        if (path && cookie.path != *path) continue;
        doomed->push_back(std::move(cookie));
      }
      DeleteNext(session, doomed, 0, token);
    });
  }
  // 逐个删除(平台的删除是异步的), 全部完成后应答个数。
  void DeleteNext(const std::string& session, std::shared_ptr<std::vector<Cookie>> doomed, size_t index,
                  uint64_t token) {
    if (index == doomed->size()) return Reply(token, Outcome::Ok(static_cast<int64_t>(doomed->size())));
    platform().DeleteCookie(session, (*doomed)[index], [=](Outcome) { DeleteNext(session, doomed, index + 1, token); });
  }

  // ---- 协议应答(JS → 原生, platform 线程) ----

  void HandleProtocolResponse(const std::string& frame) {
    Json header;
    std::string body;
    if (!DecodeFrame(frame, &header, &body)) return;
    const std::optional<double> id = header.Number("requestId");
    auto it = id ? pending_.find(static_cast<uint64_t>(*id)) : pending_.end();
    if (it == pending_.end()) return;  // 平台已取消
    Respond respond = std::move(it->second);
    pending_.erase(it);
    ProtocolResponse response;
    response.status = static_cast<int>(header.Number("status").value_or(200));
    response.headers = HeadersFrom(header["headers"]);
    response.body = std::move(body);
    respond(response);
  }

  // attach 时确定, 之后只读。
  const MomoHost* host_ = nullptr;
  // 仅 JS 线程。
  napi_ref on_event_ = nullptr;
  napi_ref on_protocol_request_ = nullptr;
  std::map<uint64_t, napi_deferred> deferreds_;  // command() 等待应答的 Promise
  uint64_t next_token_ = 0;
  // 任意线程(to_js_mutex_ 保护): 运行时销毁后为空。
  std::mutex to_js_mutex_;
  napi_threadsafe_function to_js_ = nullptr;

  // 以下全部只在 platform 线程访问。
  std::unique_ptr<WebViewPlatform> platform_;  // 第一次用到时创建
  std::map<int64_t, std::unique_ptr<WebView>> views_;
  std::map<uint64_t, Respond> pending_;  // 在途的协议请求(等待 JS 应答)
  uint64_t next_request_id_ = 0;
};

namespace {

template <napi_value (WebViewExtension::*method)(napi_env, size_t, napi_value*)>
napi_value Export(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  void* extension = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &extension);
  return (static_cast<WebViewExtension*>(extension)->*method)(env, argc, argv);
}

}  // namespace

}  // namespace momo_webview

NAPI_MODULE_INIT() {
  using momo_webview::Export;
  using momo_webview::WebViewExtension;
  auto* extension = new WebViewExtension;  // 不释放, 见类注释
  const napi_property_descriptor properties[] = {
      {"attach", nullptr, &Export<&WebViewExtension::Attach>, nullptr, nullptr, nullptr, napi_default, extension},
      {"command", nullptr, &Export<&WebViewExtension::Command>, nullptr, nullptr, nullptr, napi_default, extension},
      {"respondToProtocolRequest", nullptr, &Export<&WebViewExtension::RespondToProtocolRequest>, nullptr,
       nullptr, nullptr, napi_default, extension},
  };
  napi_define_properties(env, exports, 3, properties);
  return exports;
}
