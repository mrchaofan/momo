// momo.webview 的 Windows 实现: WebView2(webview_platform.h)。未在 Windows 上编译运行验证。
//
// 宿主契约(momo_host.h "Windows 的平台视图"): 平台视图是 MomoWinPlatformView;WebView2 以 visual hosting 方式
// 创建(ICoreWebView2CompositionController), 把自己的 visual 挂在宿主给出的挂载点(RootVisualTarget)下, 由宿主与
// momo 画面同帧合成。输入由宿主决定接收方后经 mouse 交来(SendMouseInput);光标经 cursor 取用;键盘焦点经
// MoveFocus 取得, 用户点击取得焦点时报告 focused;网页持有焦点时的快捷键先交给宿主(AcceleratorKeyPressed →
// key_down, 应用菜单优先)。WebView 不挂在任何窗口时, 它的父窗口是一个隐藏的停放窗口(换窗口 = 改 ParentWindow 与
// RootVisualTarget, 页面不重新加载)。
//
// 环境: 进程一个 ICoreWebView2Environment(第一个 WebView 或会话操作时异步创建), 数据目录
//   %LOCALAPPDATA%\<exe 名>\WebView2(每个应用一份, 与 momo 的缓存目录同一约定)。自定义协议必须在创建环境时
//   登记(CustomSchemeRegistration): 登记的是此前声明过的全部协议(DeclareScheme, 即 JS 的 registerProtocol);
//   环境创建之后才声明的协议在本进程里打不开(记录日志)。
// 会话 → WebView2 profile: default = 默认 profile;persistent:<name> = 按名字派生的 profile;ephemeral:<id> =
//   InPrivate profile。会话操作(cookie、清除数据)不依赖 WebView: 每个用到的会话有一个隐藏的 WebView(keeper)
//   提供它的 profile, 无痕会话 dispose 时关闭。
// 与 macOS 的差异: loadHtml 的 baseUrl 不生效(NavigateToString 的文档源为 about:blank, 消息的 origin 为 "null");
//   加载进度只有 0.1(开始)/ 0.5(内容开始加载)/ 1(完成)三档;页内查找用页面的 window.find。
// 线程: 全部在 platform 线程(STA, WinPlatformIntegration 已 CoInitializeEx)。WebView2 的回调经本线程的消息循环到达。

#include <windows.h>

#include <shlwapi.h>
#include <wrl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include "momo/momo_host.h"
#include "webview_platform.h"

namespace momo_webview {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

// ---- 字符串 ----

std::wstring Wide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string Utf8(const wchar_t* wide) {
  if (wide == nullptr || *wide == 0) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
  return out;
}

// WebView2 以 CoTaskMemAlloc 分配、调用方释放的字符串。
class CoString {
 public:
  CoString() = default;
  ~CoString() { ::CoTaskMemFree(text_); }
  LPWSTR* out() { return &text_; }
  std::string utf8() const { return Utf8(text_); }

 private:
  LPWSTR text_ = nullptr;
  CoString(const CoString&) = delete;
  CoString& operator=(const CoString&) = delete;
};

std::string Lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::string SchemeOf(const std::string& url) {
  const size_t colon = url.find(':');
  return colon == std::string::npos ? std::string() : Lower(url.substr(0, colon));
}

// 消息发送方的源: scheme://host[:port];没有 authority 的地址(about:、data: 等)是不透明源 "null"。
std::string OriginOf(const std::string& url) {
  const size_t colon = url.find("://");
  if (colon == std::string::npos) return "null";
  const size_t end = url.find_first_of("/?#", colon + 3);
  std::string authority = url.substr(colon + 3, end == std::string::npos ? std::string::npos : end - colon - 3);
  if (const size_t at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
  return Lower(url.substr(0, colon)) + "://" + Lower(authority);
}

std::string HrText(HRESULT hr) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

// 系统 WebView 自己处理的协议(与 JS 门面的 kBuiltInSchemes 一致)。
bool IsBuiltInScheme(const std::string& scheme) {
  static const std::set<std::string> kSchemes = {"http", "https", "file", "about", "data",
                                                 "blob", "javascript", "ws", "wss", "ftp"};
  return kSchemes.count(scheme) != 0;
}

std::string ErrorText(COREWEBVIEW2_WEB_ERROR_STATUS status) {
  switch (status) {
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CLIENT_CERTIFICATE_CONTAINS_ERRORS:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID:
      return "the server's certificate is not valid";
    case COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT:
      return "cannot connect to the server";
    case COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT: return "the request timed out";
    case COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED: return "the server cannot be found";
    case COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED: return "the network connection was lost";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET:
      return "the connection was interrupted";
    default: return "navigation failed (WebErrorStatus " + std::to_string(static_cast<int>(status)) + ")";
  }
}

// 会话键 → WebView2 profile。
struct Profile {
  std::wstring name;  // 空 = 默认 profile
  bool in_private = false;
};

std::optional<Profile> ProfileFor(const std::string& session, std::string* error) {
  // profile 名只允许有限的字符: 由会话名派生一个稳定的散列(FNV-1a 64)。
  auto hashed = [](const std::string& prefix, const std::string& name) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : name) hash = (hash ^ c) * 1099511628211ull;
    wchar_t buffer[17];
    std::swprintf(buffer, 17, L"%016llx", static_cast<unsigned long long>(hash));
    return Wide(prefix) + buffer;
  };
  if (session.empty() || session == "default") return Profile{};
  if (session.rfind("persistent:", 0) == 0 && session.size() > 11) {
    return Profile{hashed("momo-", session.substr(11)), false};
  }
  if (session.rfind("ephemeral:", 0) == 0 && session.size() > 10) {
    return Profile{hashed("momo-private-", session.substr(10)), true};
  }
  *error = "invalid session " + session;
  return std::nullopt;
}

std::wstring UserDataFolder() {
  wchar_t exe[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t local[MAX_PATH] = {};
  std::filesystem::path base = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0
                                   ? std::filesystem::path(local)
                                   : std::filesystem::temp_directory_path();
  return (base / std::filesystem::path(exe).stem() / L"WebView2").wstring();
}

Cookie FromWebViewCookie(ICoreWebView2Cookie* cookie) {
  Cookie c;
  CoString name, value, domain, path;
  cookie->get_Name(name.out());
  cookie->get_Value(value.out());
  cookie->get_Domain(domain.out());
  cookie->get_Path(path.out());
  c.name = name.utf8();
  c.value = value.utf8();
  c.domain = domain.utf8();
  c.path = path.utf8().empty() ? "/" : path.utf8();
  BOOL session = TRUE, secure = FALSE, http_only = FALSE;
  cookie->get_IsSession(&session);
  if (!session) {
    double expires = 0;
    cookie->get_Expires(&expires);
    c.expires = expires;
  }
  cookie->get_IsSecure(&secure);
  cookie->get_IsHttpOnly(&http_only);
  c.secure = secure != FALSE;
  c.http_only = http_only != FALSE;
  COREWEBVIEW2_COOKIE_SAME_SITE_KIND same_site = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE;
  cookie->get_SameSite(&same_site);
  if (same_site == COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX) c.same_site = "lax";
  if (same_site == COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT) c.same_site = "strict";
  return c;
}

// 应用的初始化脚本只在主框架执行(同 WKUserScript forMainFrameOnly);间接 eval 让脚本的 var / function
// 声明仍是全局的。
std::string MainFrameOnly(const std::string& script) {
  return "if(window===window.top){(0,eval)(" + Json(script).Dump() + ");}";
}

}  // namespace

class WinWebViewPlatform;

// ---------------------------------------------------------------------------
// 一个 WebView2。创建是异步的: 就绪前的操作排队, 就绪(或失败)后按序执行。

class WinWebView final : public WebView {
 public:
  WinWebView(WinWebViewPlatform& platform, WebViewHost& host, int64_t id, WebViewParams params);
  ~WinWebView() override;

  void* native_view() override { return &native_; }
  bool LoadUrl(const std::string& url) override;
  void LoadHtml(const std::string& html, const std::string& base_url) override;
  void GoBack() override {
    WhenReady([this] { web_->GoBack(); });
  }
  void GoForward() override {
    WhenReady([this] { web_->GoForward(); });
  }
  void Reload() override {
    WhenReady([this] { web_->Reload(); });
  }
  void Stop() override {
    WhenReady([this] { web_->Stop(); });
  }
  void EvaluateJavaScript(const std::string& script, Reply reply) override;
  void Find(const std::string& text, bool backwards, bool case_sensitive, Reply reply) override;

  // 平台: 控制器创建完成(或失败, error 非空)。
  void OnCreated(ComPtr<ICoreWebView2CompositionController> composition, const std::string& error);
  // 本对象销毁后失效(异步回调据此作废)。
  std::weak_ptr<bool> alive() const { return alive_; }
  // 隐藏期间(停放)使用的父窗口。
  HWND parking() const;

 private:
  // 就绪后执行 op;创建失败时以原因调用 fail(可以为空)。
  void WhenReady(std::function<void()> op, std::function<void(const std::string&)> fail = {});
  void Setup();
  void ApplyHost();
  void EmitState();
  // 包装 WebView2 回调: 本对象销毁后到达的回调什么也不做。
  template <typename F>
  auto Guarded(F f) {
    return [alive = std::weak_ptr<bool>(alive_), f](auto&&... args) -> HRESULT {
      return alive.expired() ? S_OK : f(std::forward<decltype(args)>(args)...);
    };
  }

  // ---- MomoWinPlatformView(宿主 → 视图) ----
  static void HostAttach(void* self, void* hwnd, void* visual, const MomoWinPlatformViewSite* site);
  static void HostSetSize(void* self, int32_t width, int32_t height);
  static void HostMouse(void* self, uint32_t message, uintptr_t wparam, int32_t x, int32_t y);
  static void HostFocus(void* self);
  static void* HostCursor(void* self);

  WinWebViewPlatform& platform_;
  WebViewHost& host_;
  const int64_t id_;
  const WebViewParams params_;
  MomoWinPlatformView native_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  // 宿主
  HWND hwnd_ = nullptr;
  ComPtr<IUnknown> visual_;  // IDCompositionVisual
  std::optional<MomoWinPlatformViewSite> site_;
  RECT bounds_{0, 0, 0, 0};

  // WebView2(就绪后)
  ComPtr<ICoreWebView2CompositionController> composition_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> web_;
  std::optional<std::string> failed_;
  std::vector<std::pair<std::function<void()>, std::function<void(const std::string&)>>> pending_;

  // 页面状态
  bool loading_ = false;
  double progress_ = 0;
  std::set<uint64_t> requests_;  // 在途的自定义协议请求
};

// ---------------------------------------------------------------------------
// 环境、会话与工厂

class WinWebViewPlatform final : public WebViewPlatform {
 public:
  explicit WinWebViewPlatform(WebViewHost& host) : host_(host) {}

  ~WinWebViewPlatform() override {
    for (auto& [key, session] : sessions_) {
      if (session->keeper) session->keeper->Close();
    }
    if (parking_ != nullptr) ::DestroyWindow(parking_);
  }

  std::unique_ptr<WebView> CreateView(int64_t id, const WebViewParams& params, std::string* error) override {
    const std::optional<Profile> profile = ProfileFor(params.session, error);
    if (!profile) return nullptr;
    auto view = std::make_unique<WinWebView>(*this, host_, id, params);
    CreateController(*profile, /*composition=*/true,
                     [raw = view.get(), alive = view->alive()](ComPtr<ICoreWebView2Controller>,
                                                               ComPtr<ICoreWebView2CompositionController> composition,
                                                               const std::string& error) {
                       if (!alive.expired()) {
                         raw->OnCreated(std::move(composition), error);
                         return;
                       }
                       // 创建期间视图已被 dispose: 直接关闭。
                       ComPtr<ICoreWebView2Controller> controller;
                       if (composition != nullptr && SUCCEEDED(composition.As(&controller))) controller->Close();
                     });
    return view;
  }

  void DeclareScheme(const std::string& scheme) override {
    if (environment_ == nullptr && environment_waiters_.empty()) declared_schemes_.insert(scheme);
  }

  void GetCookies(const std::string& session, CookiesReply reply) override {
    WithSession(session, [reply](Session* s, const std::string& error) {
      if (s == nullptr) return reply({}, error);
      s->cookies->GetCookies(L"", Callback<ICoreWebView2GetCookiesCompletedHandler>(
                                          [reply](HRESULT hr, ICoreWebView2CookieList* list) -> HRESULT {
                                            std::vector<Cookie> cookies;
                                            UINT count = 0;
                                            if (FAILED(hr) || list == nullptr) {
                                              reply({}, "cannot read cookies: " + HrText(hr));
                                              return S_OK;
                                            }
                                            list->get_Count(&count);
                                            for (UINT i = 0; i < count; ++i) {
                                              ComPtr<ICoreWebView2Cookie> cookie;
                                              if (SUCCEEDED(list->GetValueAtIndex(i, &cookie))) {
                                                cookies.push_back(FromWebViewCookie(cookie.Get()));
                                              }
                                            }
                                            reply(std::move(cookies), {});
                                            return S_OK;
                                          })
                                          .Get());
    });
  }

  void SetCookie(const std::string& session, const Cookie& cookie, Reply reply) override {
    WithSession(session, [cookie, reply](Session* s, const std::string& error) {
      if (s == nullptr) return reply(Outcome::Fail(error));
      ComPtr<ICoreWebView2Cookie> c;
      HRESULT hr = s->cookies->CreateCookie(Wide(cookie.name).c_str(), Wide(cookie.value).c_str(),
                                            Wide(cookie.domain).c_str(), Wide(cookie.path).c_str(), &c);
      if (SUCCEEDED(hr)) {
        if (cookie.expires) c->put_Expires(*cookie.expires);
        c->put_IsSecure(cookie.secure);
        c->put_IsHttpOnly(cookie.http_only);
        if (cookie.same_site == "lax") c->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX);
        if (cookie.same_site == "strict") c->put_SameSite(COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT);
        hr = s->cookies->AddOrUpdateCookie(c.Get());
      }
      reply(SUCCEEDED(hr) ? Outcome::Ok() : Outcome::Fail("cannot set the cookie: " + HrText(hr)));
    });
  }

  void DeleteCookie(const std::string& session, const Cookie& cookie, Reply reply) override {
    WithSession(session, [cookie, reply](Session* s, const std::string& error) {
      if (s == nullptr) return reply(Outcome::Fail(error));
      s->cookies->DeleteCookiesWithDomainAndPath(Wide(cookie.name).c_str(), Wide(cookie.domain).c_str(),
                                                 Wide(cookie.path).c_str());
      reply(Outcome::Ok());
    });
  }

  void ClearData(const std::string& session, const std::vector<std::string>& kinds, Reply reply) override {
    int mask = kinds.empty() ? COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_PROFILE : 0;
    for (const std::string& kind : kinds) {
      if (kind == "cookies") mask |= COREWEBVIEW2_BROWSING_DATA_KINDS_COOKIES;
      if (kind == "cache") mask |= COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE;
      if (kind == "storage") mask |= COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_DOM_STORAGE;
    }
    if (mask == 0) return reply(Outcome::Ok());
    WithSession(session, [mask, reply](Session* s, const std::string& error) {
      if (s == nullptr) return reply(Outcome::Fail(error));
      const HRESULT hr = s->profile->ClearBrowsingData(
          static_cast<COREWEBVIEW2_BROWSING_DATA_KINDS>(mask),
          Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>([reply](HRESULT hr) -> HRESULT {
            reply(SUCCEEDED(hr) ? Outcome::Ok() : Outcome::Fail("cannot clear the data: " + HrText(hr)));
            return S_OK;
          }).Get());
      if (FAILED(hr)) reply(Outcome::Fail("cannot clear the data: " + HrText(hr)));
    });
  }

  void DisposeSession(const std::string& session) override {
    if (session.rfind("ephemeral:", 0) != 0) return;
    auto it = sessions_.find(session);
    if (it == sessions_.end()) return;
    if (it->second->keeper) it->second->keeper->Close();
    sessions_.erase(it);  // 仍在创建的 keeper 完成时发现会话已不在, 直接关闭
  }

  ICoreWebView2Environment* environment() const { return environment_.Get(); }
  bool scheme_registered(const std::string& scheme) const { return declared_schemes_.count(scheme) != 0; }

  // 不挂在任何窗口的 WebView 的父窗口(隐藏, 不出现在任务栏)。
  HWND parking() {
    if (parking_ == nullptr) {
      WNDCLASSEXW wc{};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = &::DefWindowProcW;
      wc.hInstance = ::GetModuleHandleW(nullptr);
      wc.lpszClassName = L"MomoWebViewParking";
      ::RegisterClassExW(&wc);
      parking_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                   wc.hInstance, nullptr);
    }
    return parking_;
  }

 private:
  // 会话: 一个 profile 及其 cookie 管理器;profile 由一个隐藏的 WebView(keeper)提供。
  struct Session {
    ComPtr<ICoreWebView2Controller> keeper;
    ComPtr<ICoreWebView2CookieManager> cookies;
    ComPtr<ICoreWebView2Profile2> profile;
    std::string error;  // 创建失败的原因
    bool ready = false;
    std::vector<std::function<void(Session*, const std::string&)>> waiters;
  };

  using Created = std::function<void(ComPtr<ICoreWebView2Controller>, ComPtr<ICoreWebView2CompositionController>,
                                     const std::string& error)>;

  // 在 profile 上创建控制器(composition: visual hosting 的 WebView;否则普通控制器, 用作会话的 keeper)。
  void CreateController(const Profile& profile, bool composition, Created done) {
    WithEnvironment([this, profile, composition, done](const std::string& error) {
      if (!error.empty()) return done(nullptr, nullptr, error);
      ComPtr<ICoreWebView2Environment10> env10;
      ComPtr<ICoreWebView2ControllerOptions> options;
      HRESULT hr = environment_.As(&env10);
      if (SUCCEEDED(hr)) hr = env10->CreateCoreWebView2ControllerOptions(&options);
      if (SUCCEEDED(hr) && !profile.name.empty()) hr = options->put_ProfileName(profile.name.c_str());
      if (SUCCEEDED(hr)) hr = options->put_IsInPrivateModeEnabled(profile.in_private);
      if (FAILED(hr)) return done(nullptr, nullptr, "the WebView2 Runtime is too old (" + HrText(hr) + ")");
      if (composition) {
        hr = env10->CreateCoreWebView2CompositionControllerWithOptions(
            parking(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                [done](HRESULT hr, ICoreWebView2CompositionController* created) -> HRESULT {
                  if (FAILED(hr) || created == nullptr) {
                    done(nullptr, nullptr, "cannot create the WebView: " + HrText(hr));
                  } else {
                    done(nullptr, created, {});
                  }
                  return S_OK;
                })
                .Get());
      } else {
        hr = env10->CreateCoreWebView2ControllerWithOptions(
            parking(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [done](HRESULT hr, ICoreWebView2Controller* created) -> HRESULT {
                  if (FAILED(hr) || created == nullptr) {
                    done(nullptr, nullptr, "cannot create the WebView: " + HrText(hr));
                  } else {
                    done(created, nullptr, {});
                  }
                  return S_OK;
                })
                .Get());
      }
      if (FAILED(hr)) done(nullptr, nullptr, "cannot create the WebView: " + HrText(hr));
    });
  }

  // 环境就绪后调用 then(失败时 error 非空)。第一次调用时登记已声明的自定义协议并创建环境。
  void WithEnvironment(std::function<void(const std::string&)> then) {
    if (environment_ != nullptr || !environment_error_.empty()) return then(environment_error_);
    environment_waiters_.push_back(std::move(then));
    if (environment_waiters_.size() > 1) return;  // 创建中

    auto options = Make<CoreWebView2EnvironmentOptions>();
    std::vector<ComPtr<ICoreWebView2CustomSchemeRegistration>> registrations;
    std::vector<ICoreWebView2CustomSchemeRegistration*> raw;
    for (const std::string& scheme : declared_schemes_) {
      auto registration = Make<CoreWebView2CustomSchemeRegistration>(Wide(scheme).c_str());
      registration->put_TreatAsSecure(TRUE);       // 同 https: 安全上下文(Service Worker、剪贴板等可用)
      registration->put_HasAuthorityComponent(TRUE);  // app://localhost/… 的 host 部分决定源
      const wchar_t* any[] = {L"*"};
      registration->SetAllowedOrigins(1, any);     // 任何页面都可以向它发请求(由 WebView 的协议表限定)
      registrations.push_back(registration);
      raw.push_back(registration.Get());
    }
    ComPtr<ICoreWebView2EnvironmentOptions4> options4;
    if (!raw.empty() && SUCCEEDED(options.As(&options4))) {
      options4->SetCustomSchemeRegistrations(static_cast<UINT32>(raw.size()), raw.data());
    }
    const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr, UserDataFolder().c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT hr, ICoreWebView2Environment* environment) -> HRESULT {
              FinishEnvironment(environment, FAILED(hr) || environment == nullptr
                                                 ? "cannot create the WebView2 environment: " + HrText(hr)
                                                 : std::string());
              return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
      FinishEnvironment(nullptr, hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                                     ? "the WebView2 Runtime is not installed"
                                     : "cannot create the WebView2 environment: " + HrText(hr));
    }
  }

  void FinishEnvironment(ICoreWebView2Environment* environment, const std::string& error) {
    environment_ = environment;
    environment_error_ = error;
    if (!error.empty()) host_.Log(error);
    for (auto& waiter : std::exchange(environment_waiters_, {})) waiter(error);
  }

  // 会话就绪后调用 then(失败时 session 为空、error 非空)。
  void WithSession(const std::string& key, std::function<void(Session*, const std::string&)> then) {
    std::string error;
    const std::optional<Profile> profile = ProfileFor(key, &error);
    if (!profile) return then(nullptr, error);
    std::unique_ptr<Session>& slot = sessions_[key];
    if (slot != nullptr) {
      if (slot->ready) return then(slot->error.empty() ? slot.get() : nullptr, slot->error);
      slot->waiters.push_back(std::move(then));
      return;
    }
    slot = std::make_unique<Session>();
    slot->waiters.push_back(std::move(then));
    CreateController(*profile, /*composition=*/false,
                     [this, key](ComPtr<ICoreWebView2Controller> keeper, ComPtr<ICoreWebView2CompositionController>,
                                 const std::string& error) {
                       auto it = sessions_.find(key);
                       if (it == sessions_.end()) {  // 创建期间已被 dispose
                         if (keeper != nullptr) keeper->Close();
                         return;
                       }
                       Session& s = *it->second;
                       s.error = error;
                       ComPtr<ICoreWebView2> web;
                       ComPtr<ICoreWebView2_2> web2;
                       ComPtr<ICoreWebView2_13> web13;
                       ComPtr<ICoreWebView2Profile> profile;
                       if (s.error.empty() &&
                           (FAILED(keeper->get_CoreWebView2(&web)) || FAILED(web.As(&web2)) ||
                            FAILED(web2->get_CookieManager(&s.cookies)) || FAILED(web.As(&web13)) ||
                            FAILED(web13->get_Profile(&profile)) || FAILED(profile.As(&s.profile)))) {
                         s.error = "the WebView2 Runtime is too old for sessions";
                       }
                       s.keeper = keeper;
                       s.ready = true;
                       for (auto& waiter : std::exchange(s.waiters, {})) {
                         waiter(s.error.empty() ? &s : nullptr, s.error);
                       }
                     });
  }

  WebViewHost& host_;
  HWND parking_ = nullptr;
  ComPtr<ICoreWebView2Environment> environment_;
  std::string environment_error_;
  std::vector<std::function<void(const std::string&)>> environment_waiters_;
  std::set<std::string> declared_schemes_;  // 环境创建时登记的自定义协议
  std::map<std::string, std::unique_ptr<Session>> sessions_;
};

// ---------------------------------------------------------------------------
// WinWebView

WinWebView::WinWebView(WinWebViewPlatform& platform, WebViewHost& host, int64_t id, WebViewParams params)
    : platform_(platform),
      host_(host),
      id_(id),
      params_(std::move(params)),
      native_{sizeof(MomoWinPlatformView), this, &HostAttach, &HostSetSize, &HostMouse, &HostFocus, &HostCursor} {}

WinWebView::~WinWebView() {
  alive_.reset();
  for (const uint64_t request : requests_) host_.CancelProtocolRequest(request);
  if (controller_ != nullptr) {
    composition_->put_RootVisualTarget(nullptr);
    controller_->Close();
  }
}

HWND WinWebView::parking() const { return platform_.parking(); }

void WinWebView::WhenReady(std::function<void()> op, std::function<void(const std::string&)> fail) {
  if (web_ != nullptr) return op();
  if (failed_) {
    if (fail) fail(*failed_);
    return;
  }
  pending_.emplace_back(std::move(op), std::move(fail));
}

void WinWebView::OnCreated(ComPtr<ICoreWebView2CompositionController> composition, const std::string& error) {
  if (error.empty() && composition != nullptr) {
    composition_ = std::move(composition);
    if (FAILED(composition_.As(&controller_)) || FAILED(controller_->get_CoreWebView2(&web_))) {
      web_ = nullptr;
    }
  }
  if (web_ == nullptr) {
    failed_ = error.empty() ? "cannot create the WebView" : error;
    host_.Log(*failed_);
    for (auto& [op, fail] : std::exchange(pending_, {})) {
      if (fail) fail(*failed_);
    }
    return;
  }
  Setup();
  for (auto& [op, fail] : std::exchange(pending_, {})) op();
}

void WinWebView::Setup() {
  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(web_->get_Settings(&settings))) {
    settings->put_AreDevToolsEnabled(params_.devtools);
    settings->put_IsStatusBarEnabled(FALSE);  // 悬停链接时左下角的地址气泡: 与 WKWebView 一致, 不显示
    ComPtr<ICoreWebView2Settings2> settings2;
    if (!params_.user_agent.empty() && SUCCEEDED(settings.As(&settings2))) {
      settings2->put_UserAgent(Wide(params_.user_agent).c_str());
    }
  }
  // 文档创建时注入的脚本(仅主框架): 先是消息桥, 再是应用的初始化脚本。
  web_->AddScriptToExecuteOnDocumentCreated(Wide(BridgeScript("window.chrome.webview.postMessage")).c_str(), nullptr);
  for (const std::string& script : params_.init_scripts) {
    web_->AddScriptToExecuteOnDocumentCreated(Wide(MainFrameOnly(script)).c_str(), nullptr);
  }
  // 自定义协议: 请求转给 JS 的处理函数。
  for (const std::string& scheme : params_.protocols) {
    if (!platform_.scheme_registered(scheme)) {
      host_.Log("protocol \"" + scheme +
                "\" was declared after WebView2 started; it cannot be loaded in this process");
    }
    web_->AddWebResourceRequestedFilter(Wide(scheme + ":*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
  }

  EventRegistrationToken token;
  web_->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          Guarded([this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            CoString uri;
            args->get_Uri(uri.out());
            const std::string url = uri.utf8();
            const std::string scheme = SchemeOf(url);
            const bool allowed = scheme.empty() || IsBuiltInScheme(scheme) ||
                                 std::find(params_.protocols.begin(), params_.protocols.end(), scheme) !=
                                     params_.protocols.end();
            if (!allowed) {
              // 协议既不是系统 WebView 支持的, 也没有注册给这个视图: 变成一次普通的导航失败。
              args->put_Cancel(TRUE);
              host_.OnNavigationError(id_, "unsupported protocol \"" + scheme + "\" (" + url + ")");
              return S_OK;
            }
            loading_ = true;
            progress_ = 0.1;
            EmitState();
            return S_OK;
          }))
          .Get(),
      &token);
  web_->add_ContentLoading(Callback<ICoreWebView2ContentLoadingEventHandler>(
                               Guarded([this](ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs*) -> HRESULT {
                                 progress_ = 0.5;
                                 EmitState();
                                 return S_OK;
                               }))
                               .Get(),
                           &token);
  web_->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          Guarded([this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = TRUE;
            args->get_IsSuccess(&success);
            COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
            args->get_WebErrorStatus(&status);
            if (!success && status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
              host_.OnNavigationError(id_, ErrorText(status));
            }
            loading_ = false;
            progress_ = 1;
            EmitState();
            return S_OK;
          }))
          .Get(),
      &token);
  web_->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
                              Guarded([this](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                EmitState();
                                return S_OK;
                              }))
                              .Get(),
                          &token);
  web_->add_DocumentTitleChanged(Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                                     Guarded([this](ICoreWebView2*, IUnknown*) -> HRESULT {
                                       EmitState();
                                       return S_OK;
                                     }))
                                     .Get(),
                                 &token);
  web_->add_HistoryChanged(Callback<ICoreWebView2HistoryChangedEventHandler>(
                               Guarded([this](ICoreWebView2*, IUnknown*) -> HRESULT {
                                 EmitState();
                                 return S_OK;
                               }))
                               .Get(),
                           &token);
  // 页面请求新窗口: 不创建, 交给应用决定(在本视图打开、开新标签或交给系统浏览器)。
  web_->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          Guarded([this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            CoString uri;
            args->get_Uri(uri.out());
            args->put_Handled(TRUE);
            host_.OnNewWindow(id_, uri.utf8());
            return S_OK;
          }))
          .Get(),
      &token);
  web_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          Guarded([this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            // window.momo.postMessage 发来的是 JSON 文本;直接用 chrome.webview.postMessage 发来的对象取其 JSON。
            CoString text, source;
            if (FAILED(args->TryGetWebMessageAsString(text.out()))) args->get_WebMessageAsJson(text.out());
            args->get_Source(source.out());
            host_.OnMessage(id_, text.utf8(), OriginOf(source.utf8()));
            return S_OK;
          }))
          .Get(),
      &token);
  web_->add_WebResourceRequested(
      Callback<ICoreWebView2WebResourceRequestedEventHandler>(
          Guarded([this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            ComPtr<ICoreWebView2WebResourceRequest> request;
            if (FAILED(args->get_Request(&request))) return S_OK;
            ProtocolRequest r;
            CoString uri, method;
            request->get_Uri(uri.out());
            request->get_Method(method.out());
            r.url = uri.utf8();
            r.method = method.utf8().empty() ? "GET" : method.utf8();
            ComPtr<ICoreWebView2HttpRequestHeaders> headers;
            ComPtr<ICoreWebView2HttpHeadersCollectionIterator> it;
            if (SUCCEEDED(request->get_Headers(&headers)) && SUCCEEDED(headers->GetIterator(&it))) {
              BOOL has = FALSE;
              while (SUCCEEDED(it->get_HasCurrentHeader(&has)) && has) {
                CoString name, value;
                it->GetCurrentHeader(name.out(), value.out());
                r.headers[name.utf8()] = value.utf8();
                BOOL next = FALSE;
                it->MoveNext(&next);
              }
            }
            ComPtr<IStream> content;
            if (SUCCEEDED(request->get_Content(&content)) && content != nullptr) {
              char buffer[16 * 1024];
              ULONG read = 0;
              while (SUCCEEDED(content->Read(buffer, sizeof(buffer), &read)) && read > 0) r.body.append(buffer, read);
            }
            // 应答异步到达(JS 的处理函数): 延后完成这次请求。
            ComPtr<ICoreWebView2Deferral> deferral;
            args->GetDeferral(&deferral);
            ComPtr<ICoreWebView2WebResourceRequestedEventArgs> held(args);
            auto request_id = std::make_shared<uint64_t>(0);
            *request_id = host_.OnProtocolRequest(
                id_, std::move(r),
                [this, alive = std::weak_ptr<bool>(alive_), held, deferral, request_id](const ProtocolResponse& response) {
                  if (alive.expired()) return;  // 视图已销毁(请求已取消, 不会走到这里)
                  requests_.erase(*request_id);
                  std::wstring header_text;
                  for (const auto& [name, value] : response.headers) header_text += Wide(name + ": " + value) + L"\r\n";
                  ComPtr<IStream> stream;
                  stream.Attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(response.body.data()),
                                                    static_cast<UINT>(response.body.size())));
                  ComPtr<ICoreWebView2WebResourceResponse> reply;
                  if (SUCCEEDED(platform_.environment()->CreateWebResourceResponse(
                          stream.Get(), response.status, response.status == 200 ? L"OK" : L"", header_text.c_str(),
                          &reply))) {
                    held->put_Response(reply.Get());
                  }
                  deferral->Complete();
                });
            requests_.insert(*request_id);
            return S_OK;
          }))
          .Get(),
      &token);

  // 焦点、快捷键与光标: 交给宿主。
  controller_->add_GotFocus(Callback<ICoreWebView2FocusChangedEventHandler>(
                                Guarded([this](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                                  if (site_) site_->focused(site_->context);
                                  return S_OK;
                                }))
                                .Get(),
                            &token);
  controller_->add_AcceleratorKeyPressed(
      Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
          Guarded([this](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP;
            args->get_KeyEventKind(&kind);
            if (!site_ || (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                           kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)) {
              return S_OK;
            }
            UINT key = 0;
            INT lparam = 0;
            args->get_VirtualKey(&key);
            args->get_KeyEventLParam(&lparam);
            const uint32_t message = kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN ? WM_SYSKEYDOWN : WM_KEYDOWN;
            if (site_->key_down(site_->context, message, key, lparam)) args->put_Handled(TRUE);
            return S_OK;
          }))
          .Get(),
      &token);
  composition_->add_CursorChanged(Callback<ICoreWebView2CursorChangedEventHandler>(
                                      Guarded([this](ICoreWebView2CompositionController*, IUnknown*) -> HRESULT {
                                        if (site_) site_->cursor_changed(site_->context);
                                        return S_OK;
                                      }))
                                      .Get(),
                                  &token);

  controller_->put_IsVisible(TRUE);
  ApplyHost();
  controller_->put_Bounds(bounds_);
  if (params_.html) {
    LoadHtml(*params_.html, params_.base_url);
  } else if (params_.url) {
    LoadUrl(*params_.url);
  }
}

void WinWebView::ApplyHost() {
  controller_->put_ParentWindow(hwnd_ != nullptr ? hwnd_ : parking());
  composition_->put_RootVisualTarget(visual_.Get());
  if (site_) site_->commit(site_->context);
}

void WinWebView::EmitState() {
  WebViewState state;
  CoString source, title;
  web_->get_Source(source.out());
  web_->get_DocumentTitle(title.out());
  BOOL back = FALSE, forward = FALSE;
  web_->get_CanGoBack(&back);
  web_->get_CanGoForward(&forward);
  state.url = source.utf8();
  state.title = title.utf8();
  state.loading = loading_;
  state.progress = progress_;
  state.can_go_back = back != FALSE;
  state.can_go_forward = forward != FALSE;
  host_.OnStateChanged(id_, state);
}

bool WinWebView::LoadUrl(const std::string& url) {
  if (SchemeOf(url).empty()) return false;
  WhenReady([this, url] { web_->Navigate(Wide(url).c_str()); });
  return true;
}

void WinWebView::LoadHtml(const std::string& html, const std::string&) {
  WhenReady([this, html] { web_->NavigateToString(Wide(html).c_str()); });
}

void WinWebView::EvaluateJavaScript(const std::string& script, Reply reply) {
  WhenReady(
      [this, script, reply] {
        web_->ExecuteScript(Wide(script).c_str(),
                            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                                [reply](HRESULT hr, LPCWSTR json) -> HRESULT {
                                  if (FAILED(hr)) {
                                    reply(Outcome::Fail("script error: " + HrText(hr)));
                                    return S_OK;
                                  }
                                  // 结果是 JSON 文本(undefined 与无法序列化的值为 null)。
                                  const std::string text = Utf8(json);
                                  reply(Outcome::Ok(Json::Parse(text).value_or(Json(text))));
                                  return S_OK;
                                })
                                .Get());
      },
      [reply](const std::string& error) { reply(Outcome::Fail(error)); });
}

void WinWebView::Find(const std::string& text, bool backwards, bool case_sensitive, Reply reply) {
  // window.find(text, caseSensitive, backwards, wrapAround): 从当前选区继续, 选中并滚动到匹配处。
  const std::string script = std::string("!!window.find(") + Json(text).Dump() + "," +
                             (case_sensitive ? "true" : "false") + "," + (backwards ? "true" : "false") + ",true)";
  EvaluateJavaScript(script, [reply](Outcome outcome) {
    const bool* found = outcome.value.bool_value();
    reply(outcome.error.empty() ? Outcome::Ok(Json(found != nullptr && *found)) : std::move(outcome));
  });
}

// ---- MomoWinPlatformView ----

void WinWebView::HostAttach(void* self, void* hwnd, void* visual, const MomoWinPlatformViewSite* site) {
  auto* view = static_cast<WinWebView*>(self);
  const std::optional<MomoWinPlatformViewSite> old_site = view->site_;
  view->hwnd_ = static_cast<HWND>(hwnd);
  view->visual_ = static_cast<IUnknown*>(visual);
  view->site_ = site != nullptr ? std::optional<MomoWinPlatformViewSite>(*site) : std::nullopt;
  if (view->controller_ != nullptr) view->ApplyHost();
  // 原来的窗口也提交一次: 它的挂载点下已没有这个 WebView 的内容。
  if (old_site && (!view->site_ || old_site->context != view->site_->context)) old_site->commit(old_site->context);
}

void WinWebView::HostSetSize(void* self, int32_t width, int32_t height) {
  auto* view = static_cast<WinWebView*>(self);
  view->bounds_ = {0, 0, width, height};
  if (view->controller_ != nullptr) view->controller_->put_Bounds(view->bounds_);
}

void WinWebView::HostMouse(void* self, uint32_t message, uintptr_t wparam, int32_t x, int32_t y) {
  auto* view = static_cast<WinWebView*>(self);
  if (view->composition_ == nullptr) return;
  if (message == WM_MOUSELEAVE) {
    view->composition_->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE,
                                       0, POINT{0, 0});
    return;
  }
  // 鼠标事件的种类与 Win32 消息同值;滚轮与侧键的附加数据在 wparam 的高字。
  UINT32 data = 0;
  if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
    data = static_cast<UINT32>(static_cast<INT32>(GET_WHEEL_DELTA_WPARAM(wparam)));
  } else if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK) {
    data = GET_XBUTTON_WPARAM(wparam);
  }
  view->composition_->SendMouseInput(static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(message),
                                     static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(GET_KEYSTATE_WPARAM(wparam)),
                                     data, POINT{x, y});
}

void WinWebView::HostFocus(void* self) {
  auto* view = static_cast<WinWebView*>(self);
  view->WhenReady([view] { view->controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); });
}

void* WinWebView::HostCursor(void* self) {
  auto* view = static_cast<WinWebView*>(self);
  HCURSOR cursor = nullptr;
  if (view->composition_ != nullptr) view->composition_->get_Cursor(&cursor);
  return cursor;
}

std::unique_ptr<WebViewPlatform> WebViewPlatform::Create(WebViewHost& host) {
  return std::make_unique<WinWebViewPlatform>(host);
}

}  // namespace momo_webview
