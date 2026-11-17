// momo.webview 的 macOS 实现: WKWebView(webview_platform.h)。平台视图为 WKWebView 本身(NSView*)。
// 会话 → WKWebsiteDataStore: 默认存储、按名字派生标识的持久存储(macOS 14+)、非持久存储。
// 线程: 全部在主线程(platform 线程)。

#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonDigest.h>
#import <WebKit/WebKit.h>

#include <map>
#include <memory>
#include <string>

#include "webview_platform.h"

using momo_webview::Cookie;
using momo_webview::CookiesReply;
using momo_webview::Json;
using momo_webview::Outcome;
using momo_webview::ProtocolRequest;
using momo_webview::ProtocolResponse;
using momo_webview::Reply;
using momo_webview::WebViewHost;
using momo_webview::WebViewParams;
using momo_webview::WebViewState;

namespace {

NSString* NS(const std::string& text) {
  return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] ?: @"";
}

std::string Std(NSString* text) {
  const char* utf8 = text.UTF8String;
  return utf8 != nullptr ? std::string(utf8) : std::string();
}

NSData* Data(const std::string& bytes) { return [NSData dataWithBytes:bytes.data() length:bytes.size()]; }

// evaluateJavaScript 的结果 → JSON 值(无法表示的转为字符串)。
Json ToJson(id value) {
  if (value == nil || value == [NSNull null]) return Json();
  if ([value isKindOfClass:[NSString class]]) return Std(value);
  if ([value isKindOfClass:[NSNumber class]]) {
    if (CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return Json([value boolValue]);
    return Json([value doubleValue]);
  }
  if ([value isKindOfClass:[NSArray class]]) {
    Json::Array array;
    for (id item in (NSArray*)value) array.push_back(ToJson(item));
    return array;
  }
  if ([value isKindOfClass:[NSDictionary class]]) {
    Json::Object object;
    for (id key in (NSDictionary*)value) {
      if ([key isKindOfClass:[NSString class]]) object[Std(key)] = ToJson(((NSDictionary*)value)[key]);
    }
    return object;
  }
  return Std([value description]);
}

// 请求体: 自定义协议的请求体可能只以流的形式给出。
std::string RequestBody(NSURLRequest* request) {
  NSData* body = request.HTTPBody;
  if (body == nil && request.HTTPBodyStream != nil) {
    NSInputStream* stream = request.HTTPBodyStream;
    NSMutableData* data = [NSMutableData data];
    [stream open];
    uint8_t buffer[16 * 1024];
    NSInteger read = 0;
    while ((read = [stream read:buffer maxLength:sizeof(buffer)]) > 0) {
      [data appendBytes:buffer length:(NSUInteger)read];
    }
    [stream close];
    body = data;
  }
  return body != nil ? std::string(static_cast<const char*>(body.bytes), body.length) : std::string();
}

// 会话名 → 稳定的数据存储标识(同名总是同一份数据)。
NSUUID* StoreIdentifier(NSString* name) {
  NSData* utf8 = [[@"momo.webview.session:" stringByAppendingString:name] dataUsingEncoding:NSUTF8StringEncoding];
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(utf8.bytes, (CC_LONG)utf8.length, digest);
  digest[6] = (digest[6] & 0x0F) | 0x50;  // 版本 5(基于名字)
  digest[8] = (digest[8] & 0x3F) | 0x80;  // RFC 4122 变体
  return [[NSUUID alloc] initWithUUIDBytes:digest];
}

Cookie FromNSCookie(NSHTTPCookie* cookie) {
  Cookie c;
  c.name = Std(cookie.name);
  c.value = Std(cookie.value);
  c.domain = Std(cookie.domain);
  c.path = cookie.path.length > 0 ? Std(cookie.path) : "/";
  if (cookie.expiresDate != nil) c.expires = cookie.expiresDate.timeIntervalSince1970;
  c.secure = cookie.secure;
  c.http_only = cookie.HTTPOnly;
  if ([cookie.sameSitePolicy isEqualToString:NSHTTPCookieSameSiteLax]) c.same_site = "lax";
  if ([cookie.sameSitePolicy isEqualToString:NSHTTPCookieSameSiteStrict]) c.same_site = "strict";
  return c;
}

NSHTTPCookie* ToNSCookie(const Cookie& c) {
  NSMutableDictionary* properties = [NSMutableDictionary dictionary];
  properties[NSHTTPCookieName] = NS(c.name);
  properties[NSHTTPCookieValue] = NS(c.value);
  properties[NSHTTPCookieDomain] = NS(c.domain);
  properties[NSHTTPCookiePath] = NS(c.path.empty() ? "/" : c.path);
  if (c.expires) properties[NSHTTPCookieExpires] = [NSDate dateWithTimeIntervalSince1970:*c.expires];
  if (c.secure) properties[NSHTTPCookieSecure] = @"TRUE";
  if (c.http_only) properties[@"HttpOnly"] = @"TRUE";
  if (c.same_site == "lax") properties[NSHTTPCookieSameSitePolicy] = NSHTTPCookieSameSiteLax;
  if (c.same_site == "strict") properties[NSHTTPCookieSameSitePolicy] = NSHTTPCookieSameSiteStrict;
  return [NSHTTPCookie cookieWithProperties:properties];
}

NSSet<NSString*>* DataTypesFor(const std::vector<std::string>& kinds) {
  if (kinds.empty()) return [WKWebsiteDataStore allWebsiteDataTypes];
  NSMutableSet<NSString*>* types = [NSMutableSet set];
  for (const std::string& kind : kinds) {
    if (kind == "cookies") {
      [types addObject:WKWebsiteDataTypeCookies];
    } else if (kind == "cache") {
      [types addObjectsFromArray:@[
        WKWebsiteDataTypeDiskCache, WKWebsiteDataTypeMemoryCache, WKWebsiteDataTypeFetchCache,
        WKWebsiteDataTypeOfflineWebApplicationCache
      ]];
    } else if (kind == "storage") {
      [types addObjectsFromArray:@[
        WKWebsiteDataTypeLocalStorage, WKWebsiteDataTypeSessionStorage, WKWebsiteDataTypeIndexedDBDatabases,
        WKWebsiteDataTypeWebSQLDatabases, WKWebsiteDataTypeServiceWorkerRegistrations
      ]];
    }
  }
  return types;
}

void LoadUrl(WKWebView* web, NSURL* url) {
  if (url.isFileURL) {
    [web loadFileURL:url allowingReadAccessToURL:url.URLByDeletingLastPathComponent];
  } else {
    [web loadRequest:[NSURLRequest requestWithURL:url]];
  }
}

}  // namespace

// 一个 WebView 上的一个自定义协议: 请求转给插件(→ JS 的处理函数)。
__attribute__((visibility("hidden")))
@interface MomoSchemeHandler : NSObject <WKURLSchemeHandler>
- (instancetype)initWithHost:(WebViewHost*)host viewId:(int64_t)viewId;
- (void)detach;
@end

@implementation MomoSchemeHandler {
  WebViewHost* _host;
  int64_t _viewId;
  std::map<void*, uint64_t> _tasks;  // 在途的任务 → 请求号
}

- (instancetype)initWithHost:(WebViewHost*)host viewId:(int64_t)viewId {
  if ((self = [super init])) {
    _host = host;
    _viewId = viewId;
  }
  return self;
}

- (void)detach {
  for (const auto& [task, request] : _tasks) _host->CancelProtocolRequest(request);
  _tasks.clear();
  _host = nullptr;
}

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
  if (_host == nullptr) {
    [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]];
    return;
  }
  NSURLRequest* request = task.request;
  ProtocolRequest r;
  r.url = Std(request.URL.absoluteString);
  r.method = request.HTTPMethod ? Std(request.HTTPMethod) : "GET";
  for (NSString* name in request.allHTTPHeaderFields) r.headers[Std(name)] = Std(request.allHTTPHeaderFields[name]);
  r.body = RequestBody(request);
  void* key = (__bridge void*)task;
  MomoSchemeHandler* __weak weak = self;
  _tasks[key] = _host->OnProtocolRequest(_viewId, std::move(r), [weak, task, key](const ProtocolResponse& response) {
    [weak forgetTask:key];
    [MomoSchemeHandler respond:task with:response];
  });
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
  auto it = _tasks.find((__bridge void*)task);
  if (it == _tasks.end()) return;
  if (_host != nullptr) _host->CancelProtocolRequest(it->second);
  _tasks.erase(it);
}

- (void)forgetTask:(void*)key {
  _tasks.erase(key);
}

+ (void)respond:(id<WKURLSchemeTask>)task with:(const ProtocolResponse&)response {
  NSMutableDictionary<NSString*, NSString*>* fields = [NSMutableDictionary dictionary];
  for (const auto& [name, value] : response.headers) fields[NS(name)] = NS(value);
  fields[@"Content-Length"] = [NSString stringWithFormat:@"%lu", (unsigned long)response.body.size()];
  NSHTTPURLResponse* http = [[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                                        statusCode:response.status
                                                       HTTPVersion:@"HTTP/1.1"
                                                      headerFields:fields];
  // 任务可能已被 WebKit 停止(与 stopURLSchemeTask 竞争): 此时这些调用抛异常, 忽略即可。
  @try {
    [task didReceiveResponse:http];
    [task didReceiveData:Data(response.body)];
    [task didFinish];
  } @catch (NSException*) {
  }
}

@end

// WKUserContentController 强引用 handler: 用弱代理打破 webView → controller → entry → webView 的环。
__attribute__((visibility("hidden")))
@interface MomoWeakScriptHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, weak) id<WKScriptMessageHandler> target;
@end

@implementation MomoWeakScriptHandler
- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
  [self.target userContentController:controller didReceiveScriptMessage:message];
}
@end

// 每个 WebView 一个: 导航/界面/脚本消息代理 + KVO 状态上报。
__attribute__((visibility("hidden")))
@interface MomoWebViewEntry : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler>
@property(nonatomic, readonly) WKWebView* webView;
- (instancetype)initWithId:(int64_t)viewId
                    params:(const WebViewParams&)params
                     store:(WKWebsiteDataStore*)store
                      host:(WebViewHost*)host;
- (void)teardown;
@end

static void* kStateContext = &kStateContext;

@implementation MomoWebViewEntry {
  WebViewHost* _host;
  int64_t _viewId;
  MomoWeakScriptHandler* _scriptHandler;
  NSMutableArray<MomoSchemeHandler*>* _schemeHandlers;
}

+ (NSArray<NSString*>*)observedKeys {
  return @[ @"URL", @"title", @"loading", @"estimatedProgress", @"canGoBack", @"canGoForward" ];
}

- (instancetype)initWithId:(int64_t)viewId
                    params:(const WebViewParams&)params
                     store:(WKWebsiteDataStore*)store
                      host:(WebViewHost*)host {
  if ((self = [super init])) {
    _viewId = viewId;
    _host = host;
    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    config.websiteDataStore = store;

    // 自定义协议: 必须在创建 WKWebView 之前登记到配置上。
    _schemeHandlers = [NSMutableArray array];
    for (const std::string& name : params.protocols) {
      NSString* scheme = NS(name);
      if ([WKWebView handlesURLScheme:scheme] || [config urlSchemeHandlerForURLScheme:scheme] != nil) {
        host->Log("cannot register protocol \"" + name + "\": the scheme is built in or already registered");
        continue;
      }
      MomoSchemeHandler* handler = [[MomoSchemeHandler alloc] initWithHost:host viewId:viewId];
      [config setURLSchemeHandler:handler forURLScheme:scheme];
      [_schemeHandlers addObject:handler];
    }

    // 文档开始时注入的脚本(仅主框架): 先是消息桥, 再是应用的初始化脚本。
    WKUserContentController* content = config.userContentController;
    std::vector<std::string> scripts = {momo_webview::BridgeScript("window.webkit.messageHandlers.momo.postMessage")};
    scripts.insert(scripts.end(), params.init_scripts.begin(), params.init_scripts.end());
    for (const std::string& script : scripts) {
      [content addUserScript:[[WKUserScript alloc] initWithSource:NS(script)
                                                    injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                 forMainFrameOnly:YES]];
    }
    _scriptHandler = [[MomoWeakScriptHandler alloc] init];
    _scriptHandler.target = self;
    [content addScriptMessageHandler:_scriptHandler name:@"momo"];

    _webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100) configuration:config];
    _webView.navigationDelegate = self;
    _webView.UIDelegate = self;
    if (!params.user_agent.empty()) _webView.customUserAgent = NS(params.user_agent);
    if (params.devtools) _webView.inspectable = YES;
    for (NSString* key in [MomoWebViewEntry observedKeys]) {
      [_webView addObserver:self forKeyPath:key options:0 context:kStateContext];
    }

    if (params.html) {
      [_webView loadHTMLString:NS(*params.html)
                       baseURL:params.base_url.empty() ? nil : [NSURL URLWithString:NS(params.base_url)]];
    } else if (params.url) {
      NSURL* url = [NSURL URLWithString:NS(*params.url)];
      if (url != nil) LoadUrl(_webView, url);
    }
  }
  return self;
}

- (void)teardown {
  if (_host == nullptr) return;
  for (NSString* key in [MomoWebViewEntry observedKeys]) {
    [_webView removeObserver:self forKeyPath:key context:kStateContext];
  }
  [_webView stopLoading];
  _webView.navigationDelegate = nil;
  _webView.UIDelegate = nil;
  [_webView.configuration.userContentController removeScriptMessageHandlerForName:@"momo"];
  for (MomoSchemeHandler* handler in _schemeHandlers) [handler detach];
  _host = nullptr;
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
  if (context != kStateContext) {
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    return;
  }
  if (_host == nullptr) return;
  WebViewState state;
  state.url = Std(_webView.URL.absoluteString);
  state.title = Std(_webView.title);
  state.loading = _webView.loading;
  state.progress = _webView.estimatedProgress;
  state.can_go_back = _webView.canGoBack;
  state.can_go_forward = _webView.canGoForward;
  _host->OnStateChanged(_viewId, state);
}

- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
  if (_host == nullptr) return;
  // window.momo.postMessage 已把值序列化成 JSON 文本;直接用 webkit.messageHandlers 发来的对象在这里序列化。
  id body = message.body;
  std::string text = [body isKindOfClass:[NSString class]] ? Std(body) : ToJson(body).Dump();
  // 来源: 页面可以是任意网站(也可能是其中的 iframe), 应用据此判断是否可信。
  WKSecurityOrigin* source = message.frameInfo.securityOrigin;
  std::string origin = "null";  // 不透明源(loadHtml 且没有 baseUrl、data: 等)
  if (source.protocol.length > 0) {
    origin = Std(source.protocol) + "://" + Std(source.host);
    if (source.port != 0) origin += ":" + std::to_string(source.port);
  }
  _host->OnMessage(_viewId, text, origin);
}

// 页面请求新窗口: 不创建, 交给应用决定(在本视图打开、开新标签或交给系统浏览器)。
- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features {
  if (_host != nullptr) _host->OnNewWindow(_viewId, Std(action.request.URL.absoluteString));
  return nil;
}

// 地址的协议既不是系统 WebView 支持的, 也没有注册给这个视图: WebKit 会静默取消这次导航。这里把它变成
// 一次普通的导航失败(error 事件), 应用才知道网页没打开。
- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler {
  NSURL* url = action.request.URL;
  NSString* scheme = url.scheme.lowercaseString;
  if (scheme.length == 0 || [WKWebView handlesURLScheme:scheme] ||
      [webView.configuration urlSchemeHandlerForURLScheme:scheme] != nil) {
    decisionHandler(WKNavigationActionPolicyAllow);
    return;
  }
  decisionHandler(WKNavigationActionPolicyCancel);
  // 只报告主框架的导航(targetFrame 为空是新窗口请求, 由 newWindow 事件处理)。
  if (action.targetFrame.isMainFrame && _host != nullptr) {
    _host->OnNavigationError(_viewId,
                             "unsupported protocol \"" + Std(scheme) + "\" (" + Std(url.absoluteString) + ")");
  }
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
  [self reportError:error];
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
  [self reportError:error];
}

- (void)reportError:(NSError*)error {
  if (_host == nullptr || error.code == NSURLErrorCancelled) return;
  _host->OnNavigationError(_viewId, error.localizedDescription ? Std(error.localizedDescription) : "navigation failed");
}

@end

namespace momo_webview {

namespace {

class MacWebView final : public WebView {
 public:
  explicit MacWebView(MomoWebViewEntry* entry) : entry_(entry) {}
  ~MacWebView() override { [entry_ teardown]; }

  void* native_view() override { return (__bridge void*)entry_.webView; }

  bool LoadUrl(const std::string& text) override {
    NSURL* url = [NSURL URLWithString:NS(text)];
    if (url == nil || url.scheme == nil) return false;
    ::LoadUrl(web(), url);
    return true;
  }
  void LoadHtml(const std::string& html, const std::string& base_url) override {
    [web() loadHTMLString:NS(html) baseURL:base_url.empty() ? nil : [NSURL URLWithString:NS(base_url)]];
  }
  void GoBack() override { [web() goBack]; }
  void GoForward() override { [web() goForward]; }
  void Reload() override { [web() reload]; }
  void Stop() override { [web() stopLoading]; }

  void EvaluateJavaScript(const std::string& script, Reply reply) override {
    [web() evaluateJavaScript:NS(script)
            completionHandler:^(id result, NSError* error) {
              reply(error != nil ? Outcome::Fail(error.localizedDescription ? Std(error.localizedDescription)
                                                                            : "script error")
                                 : Outcome::Ok(ToJson(result)));
            }];
  }

  void Find(const std::string& text, bool backwards, bool case_sensitive, Reply reply) override {
    WKFindConfiguration* config = [[WKFindConfiguration alloc] init];
    config.backwards = backwards;
    config.caseSensitive = case_sensitive;
    config.wraps = YES;
    [web() findString:NS(text)
        withConfiguration:config
        completionHandler:^(WKFindResult* result) {
          reply(Outcome::Ok(Json(static_cast<bool>(result.matchFound))));
        }];
  }

 private:
  WKWebView* web() const { return entry_.webView; }

  MomoWebViewEntry* entry_;
};

class MacWebViewPlatform final : public WebViewPlatform {
 public:
  explicit MacWebViewPlatform(WebViewHost& host) : host_(host) {}

  // WKWebView 在每个 WebView 的配置上登记它可用的协议(创建参数 protocols), 不需要预先声明。
  void DeclareScheme(const std::string&) override {}

  std::unique_ptr<WebView> CreateView(int64_t id, const WebViewParams& params, std::string* error) override {
    WKWebsiteDataStore* store = StoreFor(params.session, error);
    if (store == nil) return nullptr;
    return std::make_unique<MacWebView>([[MomoWebViewEntry alloc] initWithId:id params:params store:store host:&host_]);
  }

  void GetCookies(const std::string& session, CookiesReply reply) override {
    std::string error;
    WKWebsiteDataStore* store = StoreFor(session, &error);
    if (store == nil) return reply({}, error);
    [store.httpCookieStore getAllCookies:^(NSArray<NSHTTPCookie*>* all) {
      std::vector<Cookie> cookies;
      for (NSHTTPCookie* cookie in all) cookies.push_back(FromNSCookie(cookie));
      reply(std::move(cookies), {});
    }];
  }

  void SetCookie(const std::string& session, const Cookie& cookie, Reply reply) override {
    std::string error;
    WKWebsiteDataStore* store = StoreFor(session, &error);
    NSHTTPCookie* ns = ToNSCookie(cookie);
    if (store == nil || ns == nil) return reply(Outcome::Fail(store == nil ? error : "invalid cookie"));
    [store.httpCookieStore setCookie:ns
                   completionHandler:^{
                     reply(Outcome::Ok());
                   }];
  }

  // 删除与 cookie 同名、同域、同路径的那一个(取存储里的对象删除: WebKit 按对象匹配)。
  void DeleteCookie(const std::string& session, const Cookie& cookie, Reply reply) override {
    std::string error;
    WKWebsiteDataStore* store = StoreFor(session, &error);
    if (store == nil) return reply(Outcome::Fail(error));
    WKHTTPCookieStore* cookies = store.httpCookieStore;
    const Cookie target = cookie;  // block 按值捕获(不捕获引用)
    [cookies getAllCookies:^(NSArray<NSHTTPCookie*>* all) {
      for (NSHTTPCookie* candidate in all) {
        const Cookie c = FromNSCookie(candidate);
        if (c.name == target.name && c.domain == target.domain && c.path == target.path) {
          [cookies deleteCookie:candidate
              completionHandler:^{
                reply(Outcome::Ok());
              }];
          return;
        }
      }
      reply(Outcome::Ok());
    }];
  }

  void ClearData(const std::string& session, const std::vector<std::string>& kinds, Reply reply) override {
    std::string error;
    WKWebsiteDataStore* store = StoreFor(session, &error);
    if (store == nil) return reply(Outcome::Fail(error));
    [store removeDataOfTypes:DataTypesFor(kinds)
               modifiedSince:[NSDate distantPast]
           completionHandler:^{
             reply(Outcome::Ok());
           }];
  }

  void DisposeSession(const std::string& session) override {
    if (session.rfind("ephemeral:", 0) == 0) [stores_ removeObjectForKey:NS(session)];
  }

 private:
  // 会话键 → 数据存储。失败(键非法、系统不支持)返回 nil 并给出原因。
  WKWebsiteDataStore* StoreFor(const std::string& session, std::string* error) {
    if (session.empty() || session == "default") return [WKWebsiteDataStore defaultDataStore];
    NSString* key = NS(session);
    WKWebsiteDataStore* store = stores_[key];
    if (store != nil) return store;
    if ([key hasPrefix:@"ephemeral:"] && key.length > 10) {
      store = [WKWebsiteDataStore nonPersistentDataStore];
    } else if ([key hasPrefix:@"persistent:"] && key.length > 11) {
      if (@available(macOS 14.0, *)) {
        store = [WKWebsiteDataStore dataStoreForIdentifier:StoreIdentifier([key substringFromIndex:11])];
      } else {
        *error = "named persistent sessions require macOS 14 or later";
        return nil;
      }
    }
    if (store == nil) {
      *error = "invalid session " + session;
      return nil;
    }
    // 持有它: 数据存储对象被释放后, 尚未完成的读写会丢失。无痕会话到 session.dispose 为止。
    stores_[key] = store;
    return store;
  }

  WebViewHost& host_;
  NSMutableDictionary<NSString*, WKWebsiteDataStore*>* stores_ = [NSMutableDictionary dictionary];
};

}  // namespace

std::unique_ptr<WebViewPlatform> WebViewPlatform::Create(WebViewHost& host) {
  return std::make_unique<MacWebViewPlatform>(host);
}

}  // namespace momo_webview
