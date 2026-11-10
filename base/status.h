#pragma once

// Status / StatusOr<T>: 全层禁异常下的错误传递(参考 absl)。header-only, 属于 momo_base。

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "base/logging.h"

namespace momo {

enum class StatusCode {
  kOk = 0,
  kCancelled,
  kUnknown,
  kInvalidArgument,
  kNotFound,
  kAlreadyExists,
  kFailedPrecondition,
  kUnimplemented,
  kInternal,
  kUnavailable,
};

constexpr std::string_view StatusCodeName(StatusCode code) {
  switch (code) {
    case StatusCode::kOk: return "OK";
    case StatusCode::kCancelled: return "CANCELLED";
    case StatusCode::kUnknown: return "UNKNOWN";
    case StatusCode::kInvalidArgument: return "INVALID_ARGUMENT";
    case StatusCode::kNotFound: return "NOT_FOUND";
    case StatusCode::kAlreadyExists: return "ALREADY_EXISTS";
    case StatusCode::kFailedPrecondition: return "FAILED_PRECONDITION";
    case StatusCode::kUnimplemented: return "UNIMPLEMENTED";
    case StatusCode::kInternal: return "INTERNAL";
    case StatusCode::kUnavailable: return "UNAVAILABLE";
  }
  return "?";
}

class [[nodiscard]] Status {
 public:
  Status() = default;  // OK
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  static Status Ok() { return Status(); }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

  std::string ToString() const {
    if (ok()) {
      return "OK";
    }
    std::string out(StatusCodeName(code_));
    out += ": ";
    out += message_;
    return out;
  }

  // 显式忽略错误(避免 [[nodiscard]] 警告)。
  void IgnoreError() const {}

  friend bool operator==(const Status& a, const Status& b) {
    return a.code_ == b.code_ && a.message_ == b.message_;
  }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

inline std::ostream& operator<<(std::ostream& os, const Status& status) {
  return os << status.ToString();
}

inline Status CancelledError(std::string msg) { return {StatusCode::kCancelled, std::move(msg)}; }
inline Status InvalidArgumentError(std::string msg) {
  return {StatusCode::kInvalidArgument, std::move(msg)};
}
inline Status NotFoundError(std::string msg) { return {StatusCode::kNotFound, std::move(msg)}; }
inline Status FailedPreconditionError(std::string msg) {
  return {StatusCode::kFailedPrecondition, std::move(msg)};
}
inline Status UnimplementedError(std::string msg) {
  return {StatusCode::kUnimplemented, std::move(msg)};
}
inline Status InternalError(std::string msg) { return {StatusCode::kInternal, std::move(msg)}; }
inline Status UnavailableError(std::string msg) {
  return {StatusCode::kUnavailable, std::move(msg)};
}

template <typename T>
class [[nodiscard]] StatusOr {
  static_assert(!std::is_same_v<std::decay_t<T>, Status>, "StatusOr<Status> is not allowed");

 public:
  // 错误态。传入 OK 状态是编程错误。
  StatusOr(Status status) : status_(std::move(status)) {  // NOLINT(implicit)
    MOMO_CHECK(!status_.ok()) << "StatusOr constructed from OK status without a value";
  }
  StatusOr(T value) : value_(std::move(value)) {}  // NOLINT(implicit)

  template <typename U, typename = std::enable_if_t<std::is_constructible_v<T, U&&> &&
                                                    !std::is_same_v<std::decay_t<U>, T> &&
                                                    !std::is_same_v<std::decay_t<U>, Status>>>
  StatusOr(U&& value) : value_(T(std::forward<U>(value))) {}  // NOLINT(implicit)

  StatusOr(StatusOr&&) = default;
  StatusOr& operator=(StatusOr&&) = default;
  StatusOr(const StatusOr&) = default;
  StatusOr& operator=(const StatusOr&) = default;

  bool ok() const { return value_.has_value(); }
  const Status& status() const { return status_; }

  T& value() & {
    MOMO_CHECK(ok()) << status_;
    return *value_;
  }
  const T& value() const& {
    MOMO_CHECK(ok()) << status_;
    return *value_;
  }
  T&& value() && {
    MOMO_CHECK(ok()) << status_;
    return std::move(*value_);
  }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(*this).value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace momo

#define MOMO_RETURN_IF_ERROR(expr)              \
  do {                                          \
    ::momo::Status momo_status_tmp_ = (expr);   \
    if (!momo_status_tmp_.ok()) {               \
      return momo_status_tmp_;                  \
    }                                           \
  } while (false)

#define MOMO_ASSIGN_OR_RETURN_IMPL(tmp, lhs, expr) \
  auto tmp = (expr);                               \
  if (!tmp.ok()) {                                 \
    return tmp.status();                           \
  }                                                \
  lhs = std::move(tmp).value()

#define MOMO_ASSIGN_OR_RETURN(lhs, expr) \
  MOMO_ASSIGN_OR_RETURN_IMPL(MOMO_CONCAT(momo_statusor_, __LINE__), lhs, expr)
