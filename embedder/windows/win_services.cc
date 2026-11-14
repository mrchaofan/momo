#include "embedder/windows/win_services.h"

#include <shellapi.h>

#include <cstring>
#include <cwchar>
#include <string>

namespace momo::embedder::windows {

namespace {

constexpr int kClipboardRetries = 4;
constexpr DWORD kClipboardRetryDelayMs = 5;

// RAII: 构造时已打开, 析构 CloseClipboard。
class ClipboardScope {
 public:
  ClipboardScope() = default;
  ~ClipboardScope() { ::CloseClipboard(); }
  ClipboardScope(const ClipboardScope&) = delete;
  ClipboardScope& operator=(const ClipboardScope&) = delete;
};

std::wstring NormalizeToCrLf(const std::wstring& text) {
  std::wstring out;
  out.reserve(text.size() + text.size() / 16);
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    if (c == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
      out.push_back(L'\r');
    }
    out.push_back(c);
  }
  return out;
}

std::wstring NormalizeToLf(const wchar_t* text, size_t length) {
  std::wstring out;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    if (text[i] == L'\r' && i + 1 < length && text[i + 1] == L'\n') {
      continue;
    }
    out.push_back(text[i]);
  }
  return out;
}

// scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"(RFC 3986)。返回小写 scheme, 无则为空。
std::string UrlScheme(std::string_view url) {
  const size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return {};
  }
  std::string scheme;
  for (size_t i = 0; i < colon; ++i) {
    const char c = url[i];
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool other = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    if (!alpha && !(i > 0 && other)) {
      return {};
    }
    scheme.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  return scheme;
}

}  // namespace

bool WinServices::OpenClipboardWithRetry() {
  for (int attempt = 0; attempt < kClipboardRetries; ++attempt) {
    if (::OpenClipboard(clipboard_owner_)) {
      return true;
    }
    ::Sleep(kClipboardRetryDelayMs);  // 其他进程正持有剪贴板
  }
  return false;
}

StatusOr<std::string> WinServices::GetClipboardText() {
  if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    return std::string();
  }
  if (!OpenClipboardWithRetry()) {
    return UnavailableError("OpenClipboard failed: " + std::to_string(::GetLastError()));
  }
  ClipboardScope scope;
  HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
  if (data == nullptr) {
    return std::string();
  }
  const auto* text = static_cast<const wchar_t*>(::GlobalLock(data));
  if (text == nullptr) {
    return InternalError("GlobalLock(clipboard) failed");
  }
  // 不信任结尾 NUL: 以分配大小为上界。
  const size_t capacity = ::GlobalSize(data) / sizeof(wchar_t);
  const size_t length = ::wcsnlen(text, capacity);
  const std::wstring normalized = NormalizeToLf(text, length);
  ::GlobalUnlock(data);
  return WideToUtf8(normalized);
}

Status WinServices::SetClipboardText(std::string_view utf8) {
  const std::wstring wide = NormalizeToCrLf(Utf8ToWide(utf8));
  const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    return InternalError("GlobalAlloc failed");
  }
  void* dest = ::GlobalLock(memory);
  if (dest == nullptr) {
    ::GlobalFree(memory);
    return InternalError("GlobalLock failed");
  }
  std::memcpy(dest, wide.c_str(), bytes);  // 含结尾 NUL
  ::GlobalUnlock(memory);

  if (!OpenClipboardWithRetry()) {
    ::GlobalFree(memory);
    return UnavailableError("OpenClipboard failed: " + std::to_string(::GetLastError()));
  }
  ClipboardScope scope;
  if (!::EmptyClipboard()) {
    ::GlobalFree(memory);
    return InternalError("EmptyClipboard failed");
  }
  if (::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    ::GlobalFree(memory);  // 失败时所有权未转移
    return InternalError("SetClipboardData failed: " + std::to_string(::GetLastError()));
  }
  return Status::Ok();  // 成功后内存归系统所有, 不得释放
}

Status WinServices::OpenUrl(std::string_view url) {
  const std::string scheme = UrlScheme(url);
  if (scheme.size() < 2) {
    return InvalidArgumentError("Invalid URL (missing scheme)");
  }
  if (scheme == "file") {
    return InvalidArgumentError("file: URLs are not allowed");
  }
  const std::wstring wide = Utf8ToWide(url);
  if (wide.empty()) {
    return InvalidArgumentError("Invalid URL");
  }
  HINSTANCE result =
      ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  // 返回值 > 32 表示成功(历史兼容的 HINSTANCE 语义)。
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    return InternalError("ShellExecuteW failed: " +
                         std::to_string(static_cast<long long>(reinterpret_cast<INT_PTR>(result))));
  }
  return Status::Ok();
}

}  // namespace momo::embedder::windows
