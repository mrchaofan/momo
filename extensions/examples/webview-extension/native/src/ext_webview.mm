// ext_webview: 系统 WebView(macOS WKWebView)作为平台视图嵌入 momo(docs/05 §9、docs/07 §3)。
// 只用 C ABI(经 momo_extension.hpp), 不链接引擎任何符号。接口设计与 WebView2 映射见本扩展的 README。
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
// 命令通道 "momo.webview"(JS → 原生, JSON, platform 线程), 回复 {"result": <any>} 或 {"error": <string>}:
//   视图命令 {"id": <viewId>, "method", ...}:
//       loadUrl {url} | loadHtml {html, baseUrl?} | goBack | goForward | reload | stop |
//       evaluateJavaScript {script} | getState
//   会话命令 {"session": <会话键>, "method", ...}:
//       cookies.get {url?} → [cookie] | cookies.set {cookie} | cookies.delete {name, url?, domain?, path?} |
//       cookies.clear | session.clearData {kinds?: ["cookies"|"cache"|"storage"]} | session.dispose
//   cookie: {"name", "value", "domain", "path", "expires": <Unix 秒>|null, "secure", "httpOnly",
//            "sameSite": "lax"|"strict"|null};cookies.set 可用 "url" 代替 "domain"。
//
// 事件通道 "momo.webview/events"(原生 → JS, JSON):
//   {"id", "event": "state", "url", "title", "loading", "progress", "canGoBack", "canGoForward"}
//   {"id", "event": "message", "body", "origin"}   页面里 window.momo.postMessage(value);body 为 JSON 文本,
//                                                  origin 为发送方的源("app://localhost";不透明源为 "null")
//   {"id", "event": "error", "message"}   导航失败
//   {"id", "event": "newWindow", "url"}   页面请求新窗口(target=_blank / window.open);原生不创建
//
// 协议通道 "momo.webview/protocol"(双向, 二进制帧: uint32 LE 头长度 + 头 JSON + 正文字节):
//   原生 → JS  头 {"requestId", "viewId", "url", "method", "headers"}, 正文为请求体
//   JS → 原生  头 {"requestId", "status", "headers"} 或 {"requestId", "error"}, 正文为响应体
//
// 线程: 工厂、命令 handler、WebKit 回调都在 platform(主)线程;事件经 channel_send 发出(任意线程可用)。

#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonDigest.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <WebKit/WebKit.h>

#include <map>
#include <string>

#include "momo/momo_extension.hpp"

namespace {

constexpr char kViewType[] = "momo.webview";
constexpr char kCommandChannel[] = "momo.webview";
constexpr char kEventChannel[] = "momo.webview/events";
constexpr char kProtocolChannel[] = "momo.webview/protocol";
constexpr char kTag[] = "momo.webview";

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
    host.SetChannelHandler(kProtocolChannel, kMomoRunnerPlatform,
                           [this](momo::ext::Bytes bytes, uint64_t reply) {
                             @autoreleasepool {
                               HandleProtocolResponse(bytes, reply);
                             }
                           });
  }

  void OnDetach() override {
    // 宿主保证 on_detach 前所有视图已 dispose;这里只做兜底。
    for (auto& [id, entry] : entries_) {
      [entry teardown];
    }
    entries_.clear();
    for (NSNumber* request_id in pending_.allKeys) {
      FailTask(pending_[request_id], NSURLErrorCancelled);
    }
    [pending_ removeAllObjects];
    host_ = nullptr;
  }

  void SendEvent(NSDictionary* event) {
    if (host_ == nullptr) return;
    NSData* json = ToJson(event);
    host_->Send(kEventChannel, json.bytes, json.length);
  }

  void Log(MomoLogLevel level, NSString* message) {
    if (host_ != nullptr) host_->Log(level, kTag, message.UTF8String);
  }

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
    momo::ext::Host* host = host_;
    if (host == nullptr) {
      FailTask(task, NSURLErrorCancelled);
      return;
    }
    NSNumber* request_id = TrackTask(task);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSString* served = path;
      NSData* data = path != nil ? ReadAsset(host, path) : nil;
      if (data == nil && fallback != nil) {
        served = fallback;
        data = ReadAsset(host, fallback);
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        id<WKURLSchemeTask> live = TakeTask(request_id);
        if (live == nil) return;  // 已被 WebKit 停止, 或扩展已卸载
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

  // 把请求转给 JS 的协议处理函数;应答经协议通道回来(HandleProtocolResponse)。
  void ForwardTask(int64_t view_id, id<WKURLSchemeTask> task) {
    if (host_ == nullptr) {
      FailTask(task, NSURLErrorCancelled);
      return;
    }
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
    host_->Send(kProtocolChannel, frame.bytes, frame.length);
  }

  // 任意线程。数据直接引用宿主的缓冲区(不再拷贝), 随 NSData 释放。
  static NSData* ReadAsset(momo::ext::Host* host, NSString* path) {
    const MomoHostApi* api = host->api();
    MomoEngineRef engine = host->engine();
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (api->open_asset(engine, path.UTF8String, &bytes, &size) != kMomoOk) return nil;
    return [[NSData alloc] initWithBytesNoCopy:const_cast<uint8_t*>(bytes)
                                        length:size
                                   deallocator:^(void* pointer, NSUInteger) {
                                     api->free_asset(engine, static_cast<const uint8_t*>(pointer));
                                   }];
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

  void* Create(int64_t view_id, momo::ext::Bytes bytes) {
    @autoreleasepool {
      NSDictionary* params = FromJson(bytes.data, bytes.size);
      NSString* error = nil;
      WKWebsiteDataStore* store = StoreFor(StringField(params, @"session"), &error);
      if (store == nil) {
        Log(kMomoLogError, [@"cannot create the WebView: " stringByAppendingString:error]);
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

  void Reply(uint64_t token, NSDictionary* payload) {
    if (host_ == nullptr) return;
    NSData* json = ToJson(payload);
    host_->Reply(token, json.bytes, json.length);
  }
  void ReplyResult(uint64_t token, id result) { Reply(token, @{@"result" : result ?: [NSNull null]}); }
  void ReplyError(uint64_t token, NSString* message) { Reply(token, @{@"error" : message ?: @"error"}); }

  void HandleCommand(momo::ext::Bytes bytes, uint64_t token) {
    NSDictionary* call = FromJson(bytes.data, bytes.size);
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
    MomoWebViewEntry* entry = it->second;
    WKWebView* web = entry.webView;
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
    } else if ([method isEqualToString:@"getState"]) {
      ReplyResult(token, StateOf(entry));
      return;
    } else if ([method isEqualToString:@"evaluateJavaScript"]) {
      // 异步结果: 在主线程回调里应答(reply token 可延后、恰好一次)。
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
    } else if ([method isEqualToString:@"cookies.clear"] || [method isEqualToString:@"session.clearData"]) {
      NSSet<NSString*>* types = [method isEqualToString:@"cookies.clear"]
                                    ? [NSSet setWithObject:WKWebsiteDataTypeCookies]
                                    : DataTypesFor(ArrayField(call, @"kinds"));
      [store removeDataOfTypes:types
                 modifiedSince:[NSDate distantPast]
             completionHandler:^{
               ReplyResult(token, nil);
             }];
    } else {
      ReplyError(token, [@"unknown method " stringByAppendingString:method]);
    }
  }

  // ---- 协议应答(JS → 原生) ----

  void HandleProtocolResponse(momo::ext::Bytes bytes, uint64_t token) {
    NSDictionary* header = nil;
    NSData* body = nil;
    if (DecodeFrame(bytes.data, bytes.size, &header, &body)) {
      id<WKURLSchemeTask> task = TakeTask(NumberField(header, @"requestId"));
      if (task != nil) {
        if (StringField(header, @"error") != nil) {
          FailTask(task, NSURLErrorCannotLoadFromNetwork);
        } else {
          NSDictionary* headers =
              [header[@"headers"] isKindOfClass:[NSDictionary class]] ? header[@"headers"] : @{};
          Respond(task, NumberField(header, @"status").integerValue ?: 200, headers, body);
        }
      }
    }
    if (host_ != nullptr) host_->Reply(token, nullptr, 0);
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
  // 以下全部只在 platform 线程访问。
  momo::ext::Host* host_ = nullptr;
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
        owner->Log(kMomoLogError,
                   [NSString stringWithFormat:@"cannot register protocol \"%@\": the scheme is built in "
                                              @"or already registered", scheme]);
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
