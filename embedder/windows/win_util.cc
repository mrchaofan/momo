// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。

#include "embedder/windows/win_util.h"

#include <chrono>
#include <limits>

#include "base/time.h"

namespace momo::embedder::windows {

std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty() || utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  const int src_len = static_cast<int>(utf8.size());
  const int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), src_len, nullptr, 0);
  if (len <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(len), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), src_len, out.data(), len);
  return out;
}

std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty() || wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  const int src_len = static_cast<int>(wide.size());
  const int len =
      ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), src_len, nullptr, 0, nullptr, nullptr);
  if (len <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), src_len, out.data(), len, nullptr, nullptr);
  return out;
}

int64_t NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
      .count();
}

}  // namespace momo::embedder::windows
