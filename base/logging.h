#pragma once

// 轻量日志与断言。header-only, 属于 momo_base。
//   MOMO_LOG(INFO) << "...";
//   MOMO_CHECK(cond) << "...";   // release 仍生效, 失败即 abort
//   MOMO_DCHECK(cond) << "...";  // 仅 debug 生效
// M0 直接写 stderr;后续可替换为 spdlog 后端而不改调用点。

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string_view>

#include "base/macros.h"

namespace momo {

enum class LogSeverity { kVerbose = 0, kInfo, kWarning, kError, kFatal };

class LogMessage {
 public:
  LogMessage(LogSeverity severity, const char* file, int line, const char* condition)
      : severity_(severity) {
    stream_ << "[" << SeverityName(severity) << ":" << BaseName(file) << "(" << line << ")] ";
    if (condition != nullptr) {
      stream_ << "Check failed: " << condition << ". ";
    }
  }

  ~LogMessage() {
    stream_ << "\n";
    const std::string text = stream_.str();
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
    if (severity_ == LogSeverity::kFatal) {
      std::abort();
    }
  }

  std::ostream& stream() { return stream_; }

  static constexpr bool ShouldLog(LogSeverity severity) {
#ifdef NDEBUG
    return severity >= LogSeverity::kInfo;
#else
    return severity >= LogSeverity::kVerbose;
#endif
  }

 private:
  static constexpr const char* SeverityName(LogSeverity severity) {
    switch (severity) {
      case LogSeverity::kVerbose: return "VERBOSE";
      case LogSeverity::kInfo: return "INFO";
      case LogSeverity::kWarning: return "WARNING";
      case LogSeverity::kError: return "ERROR";
      case LogSeverity::kFatal: return "FATAL";
    }
    return "?";
  }

  static std::string_view BaseName(std::string_view path) {
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
  }

  LogSeverity severity_;
  std::ostringstream stream_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(LogMessage);
};

// 让 `cond ? (void)0 : Voidify() & stream` 的两个分支类型一致。
struct LogMessageVoidify {
  void operator&(std::ostream&) {}
};

}  // namespace momo

#define MOMO_LOG_SEVERITY_VERBOSE ::momo::LogSeverity::kVerbose
#define MOMO_LOG_SEVERITY_INFO ::momo::LogSeverity::kInfo
#define MOMO_LOG_SEVERITY_WARNING ::momo::LogSeverity::kWarning
#define MOMO_LOG_SEVERITY_ERROR ::momo::LogSeverity::kError
#define MOMO_LOG_SEVERITY_FATAL ::momo::LogSeverity::kFatal

#define MOMO_LAZY_STREAM(stream, condition) \
  !(condition) ? (void)0 : ::momo::LogMessageVoidify() & (stream)

#define MOMO_LOG(severity)                                                               \
  MOMO_LAZY_STREAM(::momo::LogMessage(MOMO_LOG_SEVERITY_##severity, __FILE__, __LINE__, \
                                      nullptr)                                          \
                       .stream(),                                                       \
                   ::momo::LogMessage::ShouldLog(MOMO_LOG_SEVERITY_##severity))

#define MOMO_CHECK(condition)                                                             \
  MOMO_LAZY_STREAM(                                                                       \
      ::momo::LogMessage(::momo::LogSeverity::kFatal, __FILE__, __LINE__, #condition)     \
          .stream(),                                                                      \
      !(condition))

#ifdef NDEBUG
#define MOMO_DCHECK(condition) MOMO_LAZY_STREAM(::momo::LogMessage(::momo::LogSeverity::kFatal, __FILE__, __LINE__, #condition).stream(), false && !(condition))
#else
#define MOMO_DCHECK(condition) MOMO_CHECK(condition)
#endif

#define MOMO_NOTREACHED() MOMO_CHECK(false) << "Unreachable. "
