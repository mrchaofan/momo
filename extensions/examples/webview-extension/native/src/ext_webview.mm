// ext_webview: 系统 WebView(macOS WKWebView)作为平台视图嵌入 momo(docs/05 §9、docs/07 §3)。
// 一个 Node 插件: 与 JS 之间用 Node-API, 与引擎之间用宿主能力表(momo_host.h)——注册平台视图类型、
// 把任务投到 platform 线程。不链接引擎任何符号。接口设计与 WebView2 映射见本扩展的 README。
//
// 视图类型 "momo.webview"。
//
// 创建参数(PlatformView.creationParams, JSON):
//   {"url"?, "html"?, "baseUrl"?, "session"?: <会话键>, "userAgent"?, "devtools"?: bool,
//    "initScripts"?: [string],
//    "protocols"?: [{"scheme", "assets"?: <资源目录>, "index"?, "fallback"?}]}
//   protocols: 该 WebView 可用的自定义资源协议。带 assets 的由原生直接从打包资源读取;不带的转给 JS。
//   会话键: "default" | "persistent:<name>" | "ephemeral:<id>"。
//
// 插件导出(JS 门面 js/index.ts 是唯一的调用方):
//   attach(nativeHost, {assetsDir, onEvent, onProtocolRequest})   只调用一次
//   command(json) → Promise<json>          回复 {"result": <any>} 或 {"error": <string>}
//   respondToProtocolRequest(frame)        JS 协议处理函数的应答
//
// 命令(JS → 原生, JSON):
//   视图命令 {"id": <viewId>, "method", ...}:
//       loadUrl {url} | loadHtml {html, baseUrl?} | goBack | goForward | reload | stop |
//       evaluateJavaScript {script}
//   会话命令 {"session": <会话键>, "method", ...}:
//       cookies.get {url?} → [cookie] | cookies.set {cookie} | cookies.delete {name, url?, domain?, path?} |
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
// 线程: 导出的函数与两个 JS 回调在 JS 线程;视图工厂、命令的执行、WebKit 回调都在 platform(主)线程。
//   JS → platform: post_platform_task;platform → JS: 一个 napi_threadsafe_function。

#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonDigest.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <WebKit/WebKit.h>

#include <map>
#include <memory>
#include <mutex>

#include <node_api.h>

#include "momo/momo_host.h"

namespace {

constexpr char kViewType[] = "momo.webview";

// 页面侧消息桥: window.momo.postMessage(value) 发给宿主;宿主的消息以 window.momo 上的 "message" 事件到达。
NSString* const kBridgeScript =
    @"(function(){if(window.momo)return;var t=new EventTarget();"
    @"t.postMessage=function(v){window.webkit.messageHandlers.momo.postMessage("
    @"JSON.stringify(v===undefined?null:v));};"
    @"Object.defineProperty(window,'momo',{value:t});})();";

NSData* ToJson(id object) {
  if (object == nil || ![NSJSONSerialization isValidJSONObject:object]) {
    return [@"{}" dataUsingEncoding:NSUTF8StringEncoding];
  }
  return [NSJSONSerialization dataWithJSONObject:object options:0 error:nil] ?: [NSData data];
}

NSDictionary* FromJson(NSData* data) {
  if (data.length == 0) return @{};
  id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  return [object isKindOfClass:[NSDictionary class]] ? object : @{};
}

NSDictionary* FromJson(const uint8_t* bytes, size_t size) {
  return (bytes == nullptr || size == 0) ? @{} : FromJson([NSData dataWithBytes:bytes length:size]);
}

NSString* StringField(NSDictionary* dict, NSString* key) {
  id value = dict[key];
  return [value isKindOfClass:[NSString class]] ? value : nil;
}

NSNumber* NumberField(NSDictionary* dict, NSString* key) {
  id value = dict[key];
  return [value isKindOfClass:[NSNumber class]] ? value : nil;
}

NSArray* ArrayField(NSDictionary* dict, NSString* key) {
  id value = dict[key];
  return [value isKindOfClass:[NSArray class]] ? value : nil;
}

// evaluateJavaScript 的结果转成可 JSON 序列化的值。
id JsonSafe(id value) {
  if (value == nil || value == [NSNull null]) return [NSNull null];
  if ([value isKindOfClass:[NSString class]] || [value isKindOfClass:[NSNumber class]]) return value;
  if ([NSJSONSerialization isValidJSONObject:@[ value ]]) return value;
  return [value description];
}

// 协议通道的帧: uint32 LE 头长度 + 头 JSON + 正文。
NSData* EncodeFrame(NSDictionary* header, NSData* body) {
  NSData* json = ToJson(header);
  const uint32_t length = (uint32_t)json.length;
  const uint8_t prefix[4] = {(uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16),
                             (uint8_t)(length >> 24)};
  NSMutableData* frame = [NSMutableData dataWithCapacity:4 + json.length + body.length];
  [frame appendBytes:prefix length:4];
  [frame appendData:json];
  if (body != nil) [frame appendData:body];
  return frame;
}

bool DecodeFrame(const uint8_t* bytes, size_t size, NSDictionary** header, NSData** body) {
  if (bytes == nullptr || size < 4) return false;
  const size_t length = (size_t)bytes[0] | ((size_t)bytes[1] << 8) | ((size_t)bytes[2] << 16) |
                        ((size_t)bytes[3] << 24);
  if (length > size - 4) return false;
  *header = FromJson(bytes + 4, length);
  *body = [NSData dataWithBytes:bytes + 4 + length length:size - 4 - length];
  return true;
}

// 请求体: 自定义协议的请求体可能只以流的形式给出。
NSData* RequestBody(NSURLRequest* request) {
  if (request.HTTPBody != nil) return request.HTTPBody;
  NSInputStream* stream = request.HTTPBodyStream;
  if (stream == nil) return nil;
  NSMutableData* data = [NSMutableData data];
  [stream open];
  uint8_t buffer[16 * 1024];
  NSInteger read = 0;
  while ((read = [stream read:buffer maxLength:sizeof(buffer)]) > 0) {
    [data appendBytes:buffer length:(NSUInteger)read];
  }
  [stream close];
  return data;
}

NSString* MimeTypeForPath(NSString* path) {
  static NSDictionary<NSString*, NSString*>* const known = @{
    @"html" : @"text/html",
    @"htm" : @"text/html",
    @"js" : @"text/javascript",
    @"mjs" : @"text/javascript",
    @"css" : @"text/css",
    @"json" : @"application/json",
    @"map" : @"application/json",
    @"svg" : @"image/svg+xml",
    @"wasm" : @"application/wasm",
    @"txt" : @"text/plain",
    @"xml" : @"application/xml",
  };
  NSString* extension = path.pathExtension.lowercaseString;
  NSString* mime = known[extension];
  if (mime == nil && extension.length > 0) {
    mime = [UTType typeWithFilenameExtension:extension].preferredMIMEType;
  }
  return mime ?: @"application/octet-stream";
}

bool IsTextMime(NSString* mime) {
  return [mime hasPrefix:@"text/"] || [mime isEqualToString:@"application/json"] ||
         [mime isEqualToString:@"application/xml"] || [mime isEqualToString:@"image/svg+xml"];
}

// 会话名 → 稳定的数据存储标识(同名总是同一份数据)。
NSUUID* StoreIdentifier(NSString* name) {
  NSData* utf8 = [[@"momo.webview.session:" stringByAppendingString:name]
      dataUsingEncoding:NSUTF8StringEncoding];
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(utf8.bytes, (CC_LONG)utf8.length, digest);
  digest[6] = (digest[6] & 0x0F) | 0x50;  // 版本 5(基于名字)
  digest[8] = (digest[8] & 0x3F) | 0x80;  // RFC 4122 变体
  return [[NSUUID alloc] initWithUUIDBytes:digest];
}

NSDictionary* CookieToJson(NSHTTPCookie* cookie) {
  id same_site = [NSNull null];
  if ([cookie.sameSitePolicy isEqualToString:NSHTTPCookieSameSiteLax]) same_site = @"lax";
  if ([cookie.sameSitePolicy isEqualToString:NSHTTPCookieSameSiteStrict]) same_site = @"strict";
  return @{
    @"name" : cookie.name ?: @"",
    @"value" : cookie.value ?: @"",
    @"domain" : cookie.domain ?: @"",
    @"path" : cookie.path ?: @"/",
    @"expires" : cookie.expiresDate ? @(cookie.expiresDate.timeIntervalSince1970) : [NSNull null],
    @"secure" : @(cookie.secure),
    @"httpOnly" : @(cookie.HTTPOnly),
    @"sameSite" : same_site,
  };
}

NSHTTPCookie* CookieFromJson(NSDictionary* json) {
  NSString* name = StringField(json, @"name");
  NSString* domain = StringField(json, @"domain");
  NSURL* url = [NSURL URLWithString:StringField(json, @"url") ?: @""];
  if (name.length == 0 || (domain.length == 0 && url.host.length == 0)) return nil;
  NSMutableDictionary* properties = [NSMutableDictionary dictionary];
  properties[NSHTTPCookieName] = name;
  properties[NSHTTPCookieValue] = StringField(json, @"value") ?: @"";
  properties[NSHTTPCookiePath] = StringField(json, @"path") ?: @"/";
  if (domain.length > 0) {
    properties[NSHTTPCookieDomain] = domain;
  } else {
    properties[NSHTTPCookieOriginURL] = url;
  }
  NSNumber* expires = NumberField(json, @"expires");
  if (expires != nil) {
    properties[NSHTTPCookieExpires] = [NSDate dateWithTimeIntervalSince1970:expires.doubleValue];
  }
  if (NumberField(json, @"secure").boolValue) properties[NSHTTPCookieSecure] = @"TRUE";
  if (NumberField(json, @"httpOnly").boolValue) properties[@"HttpOnly"] = @"TRUE";
  NSString* same_site = StringField(json, @"sameSite");
  if ([same_site isEqualToString:@"lax"]) properties[NSHTTPCookieSameSitePolicy] = NSHTTPCookieSameSiteLax;
  if ([same_site isEqualToString:@"strict"]) {
    properties[NSHTTPCookieSameSitePolicy] = NSHTTPCookieSameSiteStrict;
  }
  return [NSHTTPCookie cookieWithProperties:properties];
}

bool DomainMatches(NSString* cookie_domain, NSString* host) {
  NSString* domain = cookie_domain.lowercaseString;
  if ([domain hasPrefix:@"."]) domain = [domain substringFromIndex:1];
  host = host.lowercaseString;
  return [host isEqualToString:domain] || [host hasSuffix:[@"." stringByAppendingString:domain]];
}

bool PathMatches(NSString* cookie_path, NSString* path) {
  if (cookie_path.length == 0 || [cookie_path isEqualToString:@"/"]) return true;
  if (path.length == 0) path = @"/";
  if ([path isEqualToString:cookie_path]) return true;
  if (![path hasPrefix:cookie_path]) return false;
  return [cookie_path hasSuffix:@"/"] || [path characterAtIndex:cookie_path.length] == '/';
}

// 该 cookie 是否会随对 url 的请求发送。
bool CookieAppliesTo(NSHTTPCookie* cookie, NSURL* url) {
  if (url.host == nil || !DomainMatches(cookie.domain, url.host)) return false;
  if (cookie.secure && ![url.scheme.lowercaseString isEqualToString:@"https"]) return false;
  return PathMatches(cookie.path, url.path);
}

NSSet<NSString*>* DataTypesFor(NSArray* kinds) {
  if (kinds == nil) return [WKWebsiteDataStore allWebsiteDataTypes];
  NSMutableSet<NSString*>* types = [NSMutableSet set];
  for (id kind in kinds) {
    if ([kind isEqual:@"cookies"]) {
      [types addObject:WKWebsiteDataTypeCookies];
    } else if ([kind isEqual:@"cache"]) {
      [types addObjectsFromArray:@[
        WKWebsiteDataTypeDiskCache, WKWebsiteDataTypeMemoryCache, WKWebsiteDataTypeFetchCache,
        WKWebsiteDataTypeOfflineWebApplicationCache
      ]];
    } else if ([kind isEqual:@"storage"]) {
      [types addObjectsFromArray:@[
        WKWebsiteDataTypeLocalStorage, WKWebsiteDataTypeSessionStorage,
        WKWebsiteDataTypeIndexedDBDatabases, WKWebsiteDataTypeWebSQLDatabases,
        WKWebsiteDataTypeServiceWorkerRegistrations
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

class WebViewExtension;

// 一个 WebView 上的一个自定义协议。assets 非空时由原生从打包资源应答, 否则转给 JS。
__attribute__((visibility("hidden")))
@interface MomoSchemeHandler : NSObject <WKURLSchemeHandler>
- (instancetype)initWithOwner:(WebViewExtension*)owner viewId:(int64_t)viewId spec:(NSDictionary*)spec;
- (void)detach;
@end

// 每个 WebView 一个: 导航/界面/脚本消息代理 + KVO 状态上报。
__attribute__((visibility("hidden")))
@interface MomoWebViewEntry : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler>
@property(nonatomic, readonly) int64_t viewId;
@property(nonatomic, readonly) WKWebView* webView;
- (instancetype)initWithId:(int64_t)viewId
                    params:(NSDictionary*)params
                     store:(WKWebsiteDataStore*)store
                     owner:(WebViewExtension*)owner;
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

// 发往 JS 线程的一条消息。
struct JsMessage {
  enum Kind { kReply, kEvent, kProtocolRequest };
  Kind kind;
  uint64_t token;  // kReply: command() 返回的 Promise
  NSData* data;
};

// 进程里只有一个(模块初始化时创建), 不释放: platform 线程上的状态(WebKit 对象、在途的回调)比 JS 运行时活得久。
class WebViewExtension {
 public:
  // ---- JS 线程: 插件导出的函数 ----

  // attach(nativeHost, {assetsDir, onEvent, onProtocolRequest})
  napi_value Attach(napi_env env, size_t argc, napi_value* argv) {
    const MomoHost* host = argc < 2 ? nullptr : momo_host_from_napi(env, argv[0]);
    napi_value assets_dir, on_event, on_protocol_request, name;
    NSString* assets = nil;
    if (host == nullptr || host_ != nullptr ||
        napi_get_named_property(env, argv[1], "assetsDir", &assets_dir) != napi_ok ||
        (assets = StringFrom(env, assets_dir)) == nil ||
        napi_get_named_property(env, argv[1], "onEvent", &on_event) != napi_ok ||
        napi_create_reference(env, on_event, 1, &on_event_) != napi_ok ||
        napi_get_named_property(env, argv[1], "onProtocolRequest", &on_protocol_request) != napi_ok ||
        napi_create_reference(env, on_protocol_request, 1, &on_protocol_request_) != napi_ok) {
      napi_throw_type_error(env, nullptr,
                            "attach(nativeHost, {assetsDir, onEvent, onProtocolRequest}) may be called once");
      return nullptr;
    }
    assets_dir_ = assets;
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
    NSString* json = argc < 1 ? nil : StringFrom(env, argv[0]);
    if (json == nil) {
      napi_throw_type_error(env, nullptr, "command(json) expects a string");
      return nullptr;
    }
    napi_deferred deferred;
    napi_value promise;
    napi_create_promise(env, &deferred, &promise);
    const uint64_t token = ++next_token_;
    NSDictionary* call = FromJson([json dataUsingEncoding:NSUTF8StringEncoding]);
    if (PostToPlatform(^{
          HandleCommand(call, token);
        })) {
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
    NSData* frame = [NSData dataWithBytes:bytes length:size];
    PostToPlatform(^{
      HandleProtocolResponse(frame);
    });
    return nullptr;
  }

 private:
  // ---- 两个方向的投递 ----

  // JS 线程 → platform 线程。引擎已关闭时返回 false。(关闭前投递、关闭时还没执行的任务被宿主丢弃。)
  bool PostToPlatform(dispatch_block_t block) {
    if (host_ == nullptr) return false;
    void* task = (void*)CFBridgingRetain([block copy]);
    if (host_->post_platform_task(host_, &RunBlock, task) == kMomoOk) return true;
    CFBridgingRelease(task);
    return false;
  }
  static void RunBlock(void* task) {
    @autoreleasepool {
      dispatch_block_t block = CFBridgingRelease(task);
      block();
    }
  }

  // platform 线程 → JS 线程。JS 运行时已销毁时丢弃。
  void PostToJs(JsMessage::Kind kind, uint64_t token, NSData* data) {
    std::lock_guard<std::mutex> lock(to_js_mutex_);
    if (to_js_ == nullptr) return;
    auto* message = new JsMessage{kind, token, data};
    if (napi_call_threadsafe_function(to_js_, message, napi_tsfn_nonblocking) != napi_ok) {
      delete message;
    }
  }

  // JS 线程: 运行时销毁时的清理函数。此前宿主已关闭宿主表并 dispose 全部视图(momo_host.h 的"关闭"),
  // 所以只剩一件事: 不再向 JS 投递(WebKit 的在途回调可能更晚才到)。
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
    if (env == nullptr) return;
    @autoreleasepool {
      static_cast<WebViewExtension*>(self)->Deliver(env, *message);
    }
  }
  void Deliver(napi_env env, const JsMessage& message) {
    napi_value value;
    if (message.kind == JsMessage::kProtocolRequest) {
      void* bytes = nullptr;
      napi_create_arraybuffer(env, message.data.length, &bytes, &value);
      memcpy(bytes, message.data.bytes, message.data.length);
      CallJs(env, on_protocol_request_, value);
      return;
    }
    napi_create_string_utf8(env, static_cast<const char*>(message.data.bytes), message.data.length, &value);
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
  static NSString* StringFrom(napi_env env, napi_value value) {
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return nil;
    NSMutableData* buffer = [NSMutableData dataWithLength:length + 1];
    napi_get_value_string_utf8(env, value, static_cast<char*>(buffer.mutableBytes), length + 1, &length);
    return [[NSString alloc] initWithBytes:buffer.bytes length:length encoding:NSUTF8StringEncoding];
  }

  static void* CreateView(void* self, int64_t view_id, const uint8_t* params, size_t size) {
    return static_cast<WebViewExtension*>(self)->Create(view_id, params, size);
  }
  static void DisposeView(void* self, int64_t view_id, void* native) {
    static_cast<WebViewExtension*>(self)->Dispose(view_id, native);
  }

 public:
  // ---- platform 线程 ----

  void SendEvent(NSDictionary* event) { PostToJs(JsMessage::kEvent, 0, ToJson(event)); }

  static void Log(NSString* message) { NSLog(@"[momo.webview] %@", message); }

  // ---- 自定义协议 ----

  // 在途的协议请求: 登记后才可以异步应答;WebKit 停止任务(导航取消、视图销毁)时注销, 之后不得再碰它。
  NSNumber* TrackTask(id<WKURLSchemeTask> task) {
    NSNumber* request_id = @(++next_request_id_);
    pending_[request_id] = task;
    return request_id;
  }
  id<WKURLSchemeTask> TakeTask(NSNumber* request_id) {
    id<WKURLSchemeTask> task = request_id ? pending_[request_id] : nil;
    if (task != nil) [pending_ removeObjectForKey:request_id];
    return task;
  }
  void ForgetTask(id<WKURLSchemeTask> task) {
    for (NSNumber* request_id in [pending_ allKeysForObject:task]) {
      [pending_ removeObjectForKey:request_id];
    }
  }

  // 用打包资源应答(路径相对应用的 assets 目录;path 为 nil 或不存在时改用 fallback, 都没有则 404)。
  // 文件在后台线程读取, 不占用 platform 线程。
  void ServeAsset(id<WKURLSchemeTask> task, NSString* path, NSString* fallback) {
    NSString* root = assets_dir_;
    NSNumber* request_id = TrackTask(task);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSString* served = path;
      NSData* data = path != nil ? ReadAsset(root, path) : nil;
      if (data == nil && fallback != nil) {
        served = fallback;
        data = ReadAsset(root, fallback);
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        id<WKURLSchemeTask> live = TakeTask(request_id);
        if (live == nil) return;  // 已被 WebKit 停止
        if (data == nil) {
          Respond(live, 404, @{@"Content-Type" : @"text/plain; charset=utf-8"},
                  [@"Not Found" dataUsingEncoding:NSUTF8StringEncoding]);
          return;
        }
        NSString* mime = MimeTypeForPath(served);
        if (IsTextMime(mime)) mime = [mime stringByAppendingString:@"; charset=utf-8"];
        Respond(live, 200, @{@"Content-Type" : mime}, data);
      });
    });
  }

  // 把请求转给 JS 的协议处理函数;应答经 respondToProtocolRequest 回来(HandleProtocolResponse)。
  void ForwardTask(int64_t view_id, id<WKURLSchemeTask> task) {
    NSNumber* request_id = TrackTask(task);
    NSURLRequest* request = task.request;
    NSData* frame = EncodeFrame(@{
      @"requestId" : request_id,
      @"viewId" : @(view_id),
      @"url" : request.URL.absoluteString ?: @"",
      @"method" : request.HTTPMethod ?: @"GET",
      @"headers" : request.allHTTPHeaderFields ?: @{},
    },
                                RequestBody(request));
    PostToJs(JsMessage::kProtocolRequest, 0, frame);
  }

  // 任意线程。path 相对应用的资源目录;越出该目录返回 nil。
  static NSData* ReadAsset(NSString* root, NSString* path) {
    NSString* base = root.stringByStandardizingPath;
    NSString* file = [base stringByAppendingPathComponent:path].stringByStandardizingPath;
    if (![file hasPrefix:[base stringByAppendingString:@"/"]]) return nil;
    return [NSData dataWithContentsOfFile:file options:NSDataReadingMappedIfSafe error:nil];
  }

  static void Respond(id<WKURLSchemeTask> task, NSInteger status, NSDictionary* headers, NSData* body) {
    NSMutableDictionary<NSString*, NSString*>* fields = [NSMutableDictionary dictionary];
    for (id key in headers) {
      if ([key isKindOfClass:[NSString class]] && [headers[key] isKindOfClass:[NSString class]]) {
        fields[key] = headers[key];
      }
    }
    fields[@"Content-Length"] = [NSString stringWithFormat:@"%lu", (unsigned long)body.length];
    NSHTTPURLResponse* response = [[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                                              statusCode:status
                                                             HTTPVersion:@"HTTP/1.1"
                                                            headerFields:fields];
    // 任务可能已被 WebKit 停止(与 stopURLSchemeTask 竞争): 此时这些调用抛异常, 忽略即可。
    @try {
      [task didReceiveResponse:response];
      [task didReceiveData:body ?: [NSData data]];
      [task didFinish];
    } @catch (NSException*) {
    }
  }

  static void FailTask(id<WKURLSchemeTask> task, NSInteger code) {
    @try {
      [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:code userInfo:nil]];
    } @catch (NSException*) {
    }
  }

 private:
  // ---- 视图 ----

  void* Create(int64_t view_id, const uint8_t* bytes, size_t size) {
    @autoreleasepool {
      NSDictionary* params = FromJson(bytes, size);
      NSString* error = nil;
      WKWebsiteDataStore* store = StoreFor(StringField(params, @"session"), &error);
      if (store == nil) {
        Log([@"cannot create the WebView: " stringByAppendingString:error]);
        return nullptr;
      }
      MomoWebViewEntry* entry = [[MomoWebViewEntry alloc] initWithId:view_id
                                                              params:params
                                                               store:store
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

  void Reply(uint64_t token, NSDictionary* payload) { PostToJs(JsMessage::kReply, token, ToJson(payload)); }
  void ReplyResult(uint64_t token, id result) { Reply(token, @{@"result" : result ?: [NSNull null]}); }
  void ReplyError(uint64_t token, NSString* message) { Reply(token, @{@"error" : message ?: @"error"}); }

  void HandleCommand(NSDictionary* call, uint64_t token) {
    NSString* method = StringField(call, @"method");
    if (method == nil) {
      ReplyError(token, @"missing method");
      return;
    }
    if ([method hasPrefix:@"cookies."] || [method hasPrefix:@"session."]) {
      HandleSessionCommand(call, method, token);
      return;
    }
    NSNumber* id_number = NumberField(call, @"id");
    auto it = id_number ? entries_.find(id_number.longLongValue) : entries_.end();
    if (it == entries_.end()) {
      ReplyError(token, @"unknown view id");
      return;
    }
    WKWebView* web = it->second.webView;
    if ([method isEqualToString:@"loadUrl"]) {
      NSURL* url = [NSURL URLWithString:StringField(call, @"url") ?: @""];
      if (url == nil || url.scheme == nil) {
        ReplyError(token, @"invalid url");
        return;
      }
      LoadUrl(web, url);
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
    } else if ([method isEqualToString:@"evaluateJavaScript"]) {
      // 异步结果: 在主线程回调里应答。
      [web evaluateJavaScript:StringField(call, @"script") ?: @""
            completionHandler:^(id result, NSError* error) {
              if (error != nil) {
                ReplyError(token, error.localizedDescription ?: @"script error");
              } else {
                ReplyResult(token, JsonSafe(result));
              }
            }];
      return;
    } else {
      ReplyError(token, [@"unknown method " stringByAppendingString:method]);
      return;
    }
    ReplyResult(token, nil);
  }

  // ---- 会话与 cookie ----

  // 会话键 → 数据存储。失败(键非法、系统不支持)返回 nil 并给出原因。
  WKWebsiteDataStore* StoreFor(NSString* key, NSString** error) {
    if (key == nil || [key isEqualToString:@"default"]) return [WKWebsiteDataStore defaultDataStore];
    WKWebsiteDataStore* store = stores_[key];
    if (store != nil) return store;
    if ([key hasPrefix:@"ephemeral:"] && key.length > 10) {
      store = [WKWebsiteDataStore nonPersistentDataStore];
    } else if ([key hasPrefix:@"persistent:"] && key.length > 11) {
      if (@available(macOS 14.0, *)) {
        store = [WKWebsiteDataStore dataStoreForIdentifier:StoreIdentifier([key substringFromIndex:11])];
      } else {
        *error = @"named persistent sessions require macOS 14 or later";
        return nil;
      }
    }
    if (store != nil) {
      // 持有它: 数据存储对象被释放后, 尚未完成的读写会丢失。
      stores_[key] = store;
      return store;
    }
    *error = [@"invalid session " stringByAppendingString:key];
    return nil;
  }

  void HandleSessionCommand(NSDictionary* call, NSString* method, uint64_t token) {
    NSString* key = StringField(call, @"session");
    if ([method isEqualToString:@"session.dispose"]) {
      if ([key hasPrefix:@"ephemeral:"]) [stores_ removeObjectForKey:key];
      ReplyResult(token, nil);
      return;
    }
    NSString* error = nil;
    WKWebsiteDataStore* store = StoreFor(key, &error);
    if (store == nil) {
      ReplyError(token, error);
      return;
    }
    WKHTTPCookieStore* cookies = store.httpCookieStore;
    if ([method isEqualToString:@"cookies.get"]) {
      NSString* url_text = StringField(call, @"url");
      NSURL* url = url_text ? [NSURL URLWithString:url_text] : nil;
      if (url_text != nil && url.host == nil) {
        ReplyError(token, @"invalid url");
        return;
      }
      [cookies getAllCookies:^(NSArray<NSHTTPCookie*>* all) {
        NSMutableArray* result = [NSMutableArray array];
        for (NSHTTPCookie* cookie in all) {
          if (url == nil || CookieAppliesTo(cookie, url)) [result addObject:CookieToJson(cookie)];
        }
        ReplyResult(token, result);
      }];
    } else if ([method isEqualToString:@"cookies.set"]) {
      NSDictionary* json = [call[@"cookie"] isKindOfClass:[NSDictionary class]] ? call[@"cookie"] : @{};
      NSHTTPCookie* cookie = CookieFromJson(json);
      if (cookie == nil) {
        ReplyError(token, @"invalid cookie (name and domain or url are required)");
        return;
      }
      [cookies setCookie:cookie
          completionHandler:^{
            ReplyResult(token, nil);
          }];
    } else if ([method isEqualToString:@"cookies.delete"]) {
      NSString* name = StringField(call, @"name");
      NSString* domain = StringField(call, @"domain");
      NSString* path = StringField(call, @"path");
      NSString* url_text = StringField(call, @"url");
      NSURL* url = url_text ? [NSURL URLWithString:url_text] : nil;
      [cookies getAllCookies:^(NSArray<NSHTTPCookie*>* all) {
        dispatch_group_t group = dispatch_group_create();
        NSUInteger removed = 0;
        for (NSHTTPCookie* cookie in all) {
          if (![cookie.name isEqualToString:name]) continue;
          if (url != nil && !CookieAppliesTo(cookie, url)) continue;
          if (domain != nil && !DomainMatches(cookie.domain, domain)) continue;
          if (path != nil && ![cookie.path isEqualToString:path]) continue;
          ++removed;
          dispatch_group_enter(group);
          [cookies deleteCookie:cookie
              completionHandler:^{
                dispatch_group_leave(group);
              }];
        }
        dispatch_group_notify(group, dispatch_get_main_queue(), ^{
          ReplyResult(token, @(removed));
        });
      }];
    } else if ([method isEqualToString:@"session.clearData"]) {
      [store removeDataOfTypes:DataTypesFor(ArrayField(call, @"kinds"))
                 modifiedSince:[NSDate distantPast]
             completionHandler:^{
               ReplyResult(token, nil);
             }];
    } else {
      ReplyError(token, [@"unknown method " stringByAppendingString:method]);
    }
  }

  // ---- 协议应答(JS → 原生) ----

  void HandleProtocolResponse(NSData* frame) {
    NSDictionary* header = nil;
    NSData* body = nil;
    if (DecodeFrame(static_cast<const uint8_t*>(frame.bytes), frame.length, &header, &body)) {
      id<WKURLSchemeTask> task = TakeTask(NumberField(header, @"requestId"));
      if (task != nil) {
        NSDictionary* headers =
            [header[@"headers"] isKindOfClass:[NSDictionary class]] ? header[@"headers"] : @{};
        Respond(task, NumberField(header, @"status").integerValue ?: 200, headers, body);
      }
    }
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
  // attach 时确定, 之后只读。
  const MomoHost* host_ = nullptr;
  NSString* assets_dir_ = nil;
  // 仅 JS 线程。
  napi_ref on_event_ = nullptr;
  napi_ref on_protocol_request_ = nullptr;
  std::map<uint64_t, napi_deferred> deferreds_;  // command() 等待应答的 Promise
  uint64_t next_token_ = 0;
  // 任意线程(to_js_mutex_ 保护): 运行时销毁后为空。
  std::mutex to_js_mutex_;
  napi_threadsafe_function to_js_ = nullptr;

  // 以下全部只在 platform 线程访问。
  std::map<int64_t, MomoWebViewEntry*> entries_;
  // 非默认会话的数据存储, 按会话键。一直持有: 没有 WebView 时也能读写其 cookie(无痕会话到 session.dispose 为止)。
  NSMutableDictionary<NSString*, WKWebsiteDataStore*>* stores_ = [NSMutableDictionary dictionary];
  // 在途的协议请求(等待 JS 应答, 或正在后台读取资源)。
  NSMutableDictionary<NSNumber*, id<WKURLSchemeTask>>* pending_ = [NSMutableDictionary dictionary];
  uint64_t next_request_id_ = 0;
};

// ---------------------------------------------------------------------------

@implementation MomoSchemeHandler {
  WebViewExtension* _owner;
  int64_t _viewId;
  NSString* _assets;    // 资源目录(相对应用 assets);nil 表示由 JS 处理
  NSString* _index;     // 目录请求对应的文件
  NSString* _fallback;  // 找不到资源时改用的文件(单页应用的前端路由), 可空
}

- (instancetype)initWithOwner:(WebViewExtension*)owner viewId:(int64_t)viewId spec:(NSDictionary*)spec {
  if ((self = [super init])) {
    _owner = owner;
    _viewId = viewId;
    _assets = StringField(spec, @"assets");
    _index = StringField(spec, @"index") ?: @"index.html";
    _fallback = StringField(spec, @"fallback");
  }
  return self;
}

- (void)detach {
  _owner = nullptr;
}

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
  if (_owner == nullptr) {
    WebViewExtension::FailTask(task, NSURLErrorCancelled);
    return;
  }
  if (_assets == nil) {
    _owner->ForwardTask(_viewId, task);
    return;
  }
  NSString* file = [self assetFileForURL:task.request.URL];
  _owner->ServeAsset(task, file != nil ? [self assetPath:file] : nil,
                     _fallback != nil ? [self assetPath:_fallback] : nil);
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
  if (_owner != nullptr) _owner->ForgetTask(task);
}

// URL 路径 → 资源目录内的相对文件;越出目录(..)返回 nil。
- (NSString*)assetFileForURL:(NSURL*)url {
  NSString* path = url.path ?: @"";
  const BOOL directory = path.length == 0 || [url.absoluteString hasSuffix:@"/"] || url.hasDirectoryPath;
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  for (NSString* part in [path componentsSeparatedByString:@"/"]) {
    if (part.length == 0 || [part isEqualToString:@"."]) continue;
    if ([part isEqualToString:@".."]) return nil;
    [parts addObject:part];
  }
  if (directory || parts.count == 0) [parts addObject:_index];
  return [parts componentsJoinedByString:@"/"];
}

- (NSString*)assetPath:(NSString*)file {
  return _assets.length > 0 ? [_assets stringByAppendingPathComponent:file] : file;
}

@end

// ---------------------------------------------------------------------------

static void* kStateContext = &kStateContext;

@implementation MomoWebViewEntry {
  WebViewExtension* _owner;
  MomoWeakScriptHandler* _scriptHandler;
  NSMutableArray<MomoSchemeHandler*>* _schemeHandlers;
  BOOL _observing;
}

+ (NSArray<NSString*>*)observedKeys {
  return @[ @"URL", @"title", @"loading", @"estimatedProgress", @"canGoBack", @"canGoForward" ];
}

- (instancetype)initWithId:(int64_t)viewId
                    params:(NSDictionary*)params
                     store:(WKWebsiteDataStore*)store
                     owner:(WebViewExtension*)owner {
  if ((self = [super init])) {
    _viewId = viewId;
    _owner = owner;
    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    config.websiteDataStore = store;

    // 自定义协议: 必须在创建 WKWebView 之前登记到配置上。
    _schemeHandlers = [NSMutableArray array];
    for (id spec in ArrayField(params, @"protocols")) {
      NSString* scheme = [spec isKindOfClass:[NSDictionary class]] ? StringField(spec, @"scheme") : nil;
      if (scheme == nil) continue;
      if ([WKWebView handlesURLScheme:scheme] || [config urlSchemeHandlerForURLScheme:scheme] != nil) {
        WebViewExtension::Log([NSString stringWithFormat:@"cannot register protocol \"%@\": the scheme is "
                                                         @"built in or already registered", scheme]);
        continue;
      }
      MomoSchemeHandler* handler = [[MomoSchemeHandler alloc] initWithOwner:owner viewId:viewId spec:spec];
      [config setURLSchemeHandler:handler forURLScheme:scheme];
      [_schemeHandlers addObject:handler];
    }

    // 文档开始时注入的脚本(仅主框架): 先是消息桥, 再是应用的初始化脚本。
    WKUserContentController* content = config.userContentController;
    NSMutableArray<NSString*>* scripts = [NSMutableArray arrayWithObject:kBridgeScript];
    for (id script in ArrayField(params, @"initScripts")) {
      if ([script isKindOfClass:[NSString class]]) [scripts addObject:script];
    }
    for (NSString* script in scripts) {
      [content addUserScript:[[WKUserScript alloc] initWithSource:script
                                                    injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                 forMainFrameOnly:YES]];
    }
    _scriptHandler = [[MomoWeakScriptHandler alloc] init];
    _scriptHandler.target = self;
    [content addScriptMessageHandler:_scriptHandler name:@"momo"];

    _webView = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100) configuration:config];
    _webView.navigationDelegate = self;
    _webView.UIDelegate = self;
    NSString* user_agent = StringField(params, @"userAgent");
    if (user_agent.length > 0) _webView.customUserAgent = user_agent;
    if (NumberField(params, @"devtools").boolValue) _webView.inspectable = YES;
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
      if (u != nil) LoadUrl(_webView, u);
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
  _webView.UIDelegate = nil;
  [_webView.configuration.userContentController removeScriptMessageHandlerForName:@"momo"];
  for (MomoSchemeHandler* handler in _schemeHandlers) {
    [handler detach];
  }
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
  // window.momo.postMessage 已把值序列化成 JSON 文本;直接用 webkit.messageHandlers 发来的对象在这里序列化。
  id body = message.body;
  NSString* text = [body isKindOfClass:[NSString class]] ? body : nil;
  if (text == nil && body != nil && [NSJSONSerialization isValidJSONObject:@[ body ]]) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
  }
  // 来源: 页面可以是任意网站(也可能是其中的 iframe), 应用据此判断是否可信。
  WKSecurityOrigin* source = message.frameInfo.securityOrigin;
  NSString* origin = @"null";  // 不透明源(loadHtml 且没有 baseUrl、data: 等)
  if (source.protocol.length > 0) {
    origin = [NSString stringWithFormat:@"%@://%@", source.protocol, source.host ?: @""];
    if (source.port != 0) origin = [origin stringByAppendingFormat:@":%ld", (long)source.port];
  }
  _owner->SendEvent(@{
    @"id" : @(_viewId),
    @"event" : @"message",
    @"body" : text ?: @"null",
    @"origin" : origin
  });
}

// 页面请求新窗口: 不创建, 交给应用决定(在本视图打开、开新标签或交给系统浏览器)。
- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features {
  if (_owner != nullptr) {
    _owner->SendEvent(@{
      @"id" : @(_viewId),
      @"event" : @"newWindow",
      @"url" : action.request.URL.absoluteString ?: @""
    });
  }
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
  if (action.targetFrame.isMainFrame) {
    [self reportError:[NSError errorWithDomain:NSURLErrorDomain
                                          code:NSURLErrorUnsupportedURL
                                      userInfo:@{
                                        NSLocalizedDescriptionKey : [NSString
                                            stringWithFormat:@"unsupported protocol \"%@\" (%@)", scheme,
                                                             url.absoluteString]
                                      }]];
  }
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

// ---------------------------------------------------------------------------

namespace {

template <napi_value (WebViewExtension::*method)(napi_env, size_t, napi_value*)>
napi_value Export(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  void* extension = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &extension);
  @autoreleasepool {
    return (static_cast<WebViewExtension*>(extension)->*method)(env, argc, argv);
  }
}

}  // namespace

NAPI_MODULE_INIT() {
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
