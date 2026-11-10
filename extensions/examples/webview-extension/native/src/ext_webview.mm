// ext_webview: 系统 WebView(macOS WKWebView)作为平台视图嵌入 momo(docs/05 §9、docs/07 §3)。
// 只用 C ABI(经 momo_extension.hpp), 不链接引擎任何符号。
//
// 视图类型 "momo.webview"。全部消息为 UTF-8 JSON:
//   创建参数(PlatformView.creationParams):   {"url"?: string, "html"?: string, "baseUrl"?: string,
//                                             "pages"?: {"momo://…": html}}
//   内部页面: momo:// 地址由扩展的 URL scheme handler 返回 pages 中登记的 HTML(进程内共享)。
//     与 loadHtml 不同, 它们是真实地址: 刷新、后退、前进都能回到该页面。
//   命令通道 "momo.webview"(JS → 原生, platform 线程):
//       {"id": <viewId>, "method": <name>, ...}  →  回复 {"result": <any>} 或 {"error": <string>}
//       loadUrl {url} | loadHtml {html, baseUrl?} | goBack | goForward | reload | stop |
//       evaluateJavaScript {script} | getState
//   事件通道 "momo.webview/events"(原生 → JS):
//       {"id", "event": "state", "url", "title", "loading", "progress", "canGoBack", "canGoForward"}
//       {"id", "event": "message", "body"}   页面里 window.webkit.messageHandlers.momo.postMessage(body)
//       {"id", "event": "error", "message"}  导航失败
// 线程: 工厂与命令 handler 都在 platform(主)线程;事件经 channel_send 发出(任意线程可用)。

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <map>
#include <string>

#include "momo/momo_extension.hpp"

namespace {

constexpr char kViewType[] = "momo.webview";
constexpr char kCommandChannel[] = "momo.webview";
constexpr char kEventChannel[] = "momo.webview/events";
constexpr char kTag[] = "momo.webview";

NSData* ToJson(id object) {
  if (object == nil || ![NSJSONSerialization isValidJSONObject:object]) {
    return [@"{}" dataUsingEncoding:NSUTF8StringEncoding];
  }
  return [NSJSONSerialization dataWithJSONObject:object options:0 error:nil] ?: [NSData data];
}

NSDictionary* FromJson(const uint8_t* bytes, size_t size) {
  if (bytes == nullptr || size == 0) {
    return @{};
  }
  NSData* data = [NSData dataWithBytes:bytes length:size];
  id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  return [object isKindOfClass:[NSDictionary class]] ? object : @{};
}

NSString* StringField(NSDictionary* dict, NSString* key) {
  id value = dict[key];
  return [value isKindOfClass:[NSString class]] ? value : nil;
}

// evaluateJavaScript 的结果转成可 JSON 序列化的值。
id JsonSafe(id value) {
  if (value == nil || value == [NSNull null]) return [NSNull null];
  if ([value isKindOfClass:[NSString class]] || [value isKindOfClass:[NSNumber class]]) return value;
  if ([NSJSONSerialization isValidJSONObject:@[ value ]]) return value;
  return [value description];
}

}  // namespace

// ---------------------------------------------------------------------------
// momo:// 内部页面

constexpr NSString* kInternalScheme = @"momo";

NSMutableDictionary<NSString*, NSString*>* InternalPages() {
  static NSMutableDictionary<NSString*, NSString*>* pages = [NSMutableDictionary dictionary];
  return pages;
}

__attribute__((visibility("hidden")))
@interface MomoInternalSchemeHandler : NSObject <WKURLSchemeHandler>
@end

@implementation MomoInternalSchemeHandler
- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
  NSURL* url = task.request.URL;
  NSString* key = url.absoluteString;
  if ([key hasSuffix:@"/"]) key = [key substringToIndex:key.length - 1];
  NSString* html = InternalPages()[key];
  if (html == nil) {
    [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain
                                               code:NSURLErrorFileDoesNotExist
                                           userInfo:nil]];
    return;
  }
  NSData* data = [html dataUsingEncoding:NSUTF8StringEncoding];
  NSURLResponse* response = [[NSURLResponse alloc] initWithURL:url
                                                      MIMEType:@"text/html"
                                         expectedContentLength:(NSInteger)data.length
                                              textEncodingName:@"utf-8"];
  [task didReceiveResponse:response];
  [task didReceiveData:data];
  [task didFinish];
}
- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
}
@end

class WebViewExtension;

// 每个 WebView 一个: 导航/脚本消息代理 + KVO 状态上报。
__attribute__((visibility("hidden")))
@interface MomoWebViewEntry : NSObject <WKNavigationDelegate, WKScriptMessageHandler>
@property(nonatomic, readonly) int64_t viewId;
@property(nonatomic, readonly) WKWebView* webView;
- (instancetype)initWithId:(int64_t)viewId params:(NSDictionary*)params owner:(WebViewExtension*)owner;
- (void)teardown;
@end

// WKUserContentController 强引用 handler: 用弱代理打破 webView → controller → entry → webView 的环。
__attribute__((visibility("hidden")))
@interface MomoWeakScriptHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, weak) id<WKScriptMessageHandler> target;
@end

@implementation MomoWeakScriptHandler
- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
  [self.target userContentController:controller didReceiveScriptMessage:message];
}
@end

class WebViewExtension final : public momo::ext::Extension {
 public:
  void OnAttach(momo::ext::Host& host) override {
    host_ = &host;
    if (!host.SupportsPlatformViews()) {
      host.Log(kMomoLogError, kTag, "host does not support platform views; WebView unavailable");
      return;
    }
    const MomoStatus status = host.RegisterPlatformViewFactory(
        kViewType,
        [this](int64_t view_id, momo::ext::Bytes params) -> void* { return Create(view_id, params); },
        [this](int64_t view_id, void* native) { Dispose(view_id, native); });
    if (status != kMomoOk) {
      host.Log(kMomoLogError, kTag, "failed to register the momo.webview platform view type");
      return;
    }
    host.SetChannelHandler(kCommandChannel, kMomoRunnerPlatform,
                           [this](momo::ext::Bytes bytes, uint64_t reply) {
                             @autoreleasepool {
                               HandleCommand(bytes, reply);
                             }
                           });
  }

  void OnDetach() override {
    // 宿主保证 on_detach 前所有视图已 dispose;这里只做兜底。
    for (auto& [id, entry] : entries_) {
      [entry teardown];
    }
    entries_.clear();
    host_ = nullptr;
  }

  void SendEvent(NSDictionary* event) {
    if (host_ == nullptr) return;
    NSData* json = ToJson(event);
    host_->Send(kEventChannel, json.bytes, json.length);
  }

 private:
  void* Create(int64_t view_id, momo::ext::Bytes params) {
    @autoreleasepool {
      MomoWebViewEntry* entry = [[MomoWebViewEntry alloc] initWithId:view_id
                                                              params:FromJson(params.data, params.size)
                                                               owner:this];
      entries_[view_id] = entry;
      // 所有权交给宿主借用(dispose 时 CFBridgingRelease 归还)。
      return (void*)CFBridgingRetain(entry.webView);
    }
  }

  void Dispose(int64_t view_id, void* native) {
    @autoreleasepool {
      auto it = entries_.find(view_id);
      if (it != entries_.end()) {
        [it->second teardown];
        entries_.erase(it);
      }
      if (native != nullptr) {
        CFBridgingRelease(native);
      }
    }
  }

  void Reply(uint64_t token, NSDictionary* payload) {
    NSData* json = ToJson(payload);
    host_->Reply(token, json.bytes, json.length);
  }

  void HandleCommand(momo::ext::Bytes bytes, uint64_t token) {
    NSDictionary* call = FromJson(bytes.data, bytes.size);
    NSNumber* id_number = [call[@"id"] isKindOfClass:[NSNumber class]] ? call[@"id"] : nil;
    NSString* method = StringField(call, @"method");
    auto it = id_number ? entries_.find(id_number.longLongValue) : entries_.end();
    if (method == nil || it == entries_.end()) {
      Reply(token, @{@"error" : @"unknown view id or missing method"});
      return;
    }
    MomoWebViewEntry* entry = it->second;
    WKWebView* web = entry.webView;
    if ([method isEqualToString:@"loadUrl"]) {
      NSURL* url = [NSURL URLWithString:StringField(call, @"url") ?: @""];
      if (url == nil || url.scheme == nil) {
        Reply(token, @{@"error" : @"invalid url"});
        return;
      }
      if (url.isFileURL) {
        [web loadFileURL:url allowingReadAccessToURL:url.URLByDeletingLastPathComponent];
      } else {
        [web loadRequest:[NSURLRequest requestWithURL:url]];
      }
    } else if ([method isEqualToString:@"loadHtml"]) {
      NSString* base = StringField(call, @"baseUrl");
      [web loadHTMLString:StringField(call, @"html") ?: @"" baseURL:base ? [NSURL URLWithString:base] : nil];
    } else if ([method isEqualToString:@"goBack"]) {
      [web goBack];
    } else if ([method isEqualToString:@"goForward"]) {
      [web goForward];
    } else if ([method isEqualToString:@"reload"]) {
      [web reload];
    } else if ([method isEqualToString:@"stop"]) {
      [web stopLoading];
    } else if ([method isEqualToString:@"getState"]) {
      Reply(token, @{@"result" : StateOf(entry)});
      return;
    } else if ([method isEqualToString:@"evaluateJavaScript"]) {
      // 异步结果: 在主线程回调里应答(reply token 可延后、恰好一次)。
      [web evaluateJavaScript:StringField(call, @"script") ?: @""
            completionHandler:^(id result, NSError* error) {
              if (host_ == nullptr) return;
              if (error != nil) {
                Reply(token, @{@"error" : error.localizedDescription ?: @"script error"});
              } else {
                Reply(token, @{@"result" : JsonSafe(result)});
              }
            }];
      return;
    } else {
      Reply(token, @{@"error" : [@"unknown method " stringByAppendingString:method]});
      return;
    }
    Reply(token, @{@"result" : [NSNull null]});
  }

 public:
  static NSDictionary* StateOf(MomoWebViewEntry* entry) {
    WKWebView* web = entry.webView;
    return @{
      @"id" : @(entry.viewId),
      @"event" : @"state",
      @"url" : web.URL.absoluteString ?: @"",
      @"title" : web.title ?: @"",
      @"loading" : @(web.loading),
      @"progress" : @(web.estimatedProgress),
      @"canGoBack" : @(web.canGoBack),
      @"canGoForward" : @(web.canGoForward),
    };
  }

 private:
  momo::ext::Host* host_ = nullptr;
  std::map<int64_t, MomoWebViewEntry*> entries_;  // platform 线程
};

// ---------------------------------------------------------------------------

static void* kStateContext = &kStateContext;

@implementation MomoWebViewEntry {
  WebViewExtension* _owner;
  MomoWeakScriptHandler* _scriptHandler;
  BOOL _observing;
}

+ (NSArray<NSString*>*)observedKeys {
  return @[ @"URL", @"title", @"loading", @"estimatedProgress", @"canGoBack", @"canGoForward" ];
}

- (instancetype)initWithId:(int64_t)viewId params:(NSDictionary*)params owner:(WebViewExtension*)owner {
  if ((self = [super init])) {
    _viewId = viewId;
    _owner = owner;
    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    static MomoInternalSchemeHandler* scheme_handler = [[MomoInternalSchemeHandler alloc] init];
    [config setURLSchemeHandler:scheme_handler forURLScheme:kInternalScheme];
    NSDictionary* pages = [params[@"pages"] isKindOfClass:[NSDictionary class]] ? params[@"pages"] : nil;
    for (NSString* key in pages) {
      if ([key isKindOfClass:[NSString class]] && [pages[key] isKindOfClass:[NSString class]]) {
        InternalPages()[key] = pages[key];
      }
    }
    _scriptHandler = [[MomoWeakScriptHandler alloc] init];
    _scriptHandler.target = self;
    [config.userContentController addScriptMessageHandler:_scriptHandler name:@"momo"];
    _webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100) configuration:config];
    _webView.navigationDelegate = self;
    for (NSString* key in [MomoWebViewEntry observedKeys]) {
      [_webView addObserver:self forKeyPath:key options:0 context:kStateContext];
    }
    _observing = YES;

    NSString* html = StringField(params, @"html");
    NSString* url = StringField(params, @"url");
    if (html != nil) {
      NSString* base = StringField(params, @"baseUrl");
      [_webView loadHTMLString:html baseURL:base ? [NSURL URLWithString:base] : nil];
    } else if (url != nil) {
      NSURL* u = [NSURL URLWithString:url];
      if (u != nil) [_webView loadRequest:[NSURLRequest requestWithURL:u]];
    }
  }
  return self;
}

- (void)teardown {
  if (_observing) {
    for (NSString* key in [MomoWebViewEntry observedKeys]) {
      [_webView removeObserver:self forKeyPath:key context:kStateContext];
    }
    _observing = NO;
  }
  [_webView stopLoading];
  _webView.navigationDelegate = nil;
  [_webView.configuration.userContentController removeScriptMessageHandlerForName:@"momo"];
  _owner = nullptr;
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
  if (context != kStateContext) {
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    return;
  }
  if (_owner != nullptr) {
    _owner->SendEvent(WebViewExtension::StateOf(self));
  }
}

- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
  if (_owner == nullptr) return;
  id body = message.body;
  NSString* text = [body isKindOfClass:[NSString class]] ? body : nil;
  if (text == nil && body != nil && [NSJSONSerialization isValidJSONObject:@[ body ]]) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
  }
  _owner->SendEvent(@{@"id" : @(_viewId), @"event" : @"message", @"body" : text ?: @""});
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
            withError:(NSError*)error {
  [self reportError:error];
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
  [self reportError:error];
}

- (void)reportError:(NSError*)error {
  if (_owner == nullptr || error.code == NSURLErrorCancelled) return;
  _owner->SendEvent(@{
    @"id" : @(_viewId),
    @"event" : @"error",
    @"message" : error.localizedDescription ?: @"navigation failed"
  });
}

@end

MOMO_EXTENSION(WebViewExtension, "momo.example.webview")
