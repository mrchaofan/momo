#pragma once

// 最小 JSON 解析器(RFC 8259 子集足够用于 manifest/配置;无异常)。
// 数字统一为 double;对象保持键的出现顺序;字符串为 UTF-8(\uXXXX 含代理对转为 UTF-8)。
// 线程约定: 值类型, 无共享状态。

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/status.h"

namespace momo::engine {

class JsonValue {
 public:
  using Array = std::vector<JsonValue>;
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  JsonValue() = default;
  explicit JsonValue(bool v) : v_(v) {}
  explicit JsonValue(double v) : v_(v) {}
  explicit JsonValue(std::string v) : v_(std::move(v)) {}
  explicit JsonValue(Array v) : v_(std::move(v)) {}
  explicit JsonValue(Object v) : v_(std::move(v)) {}

  bool is_null() const { return std::holds_alternative<std::monostate>(v_); }
  const bool* AsBool() const { return std::get_if<bool>(&v_); }
  const double* AsNumber() const { return std::get_if<double>(&v_); }
  const std::string* AsString() const { return std::get_if<std::string>(&v_); }
  const Array* AsArray() const { return std::get_if<Array>(&v_); }
  const Object* AsObject() const { return std::get_if<Object>(&v_); }

  // 对象成员(非对象或缺键返回 nullptr)。
  const JsonValue* Get(std::string_view key) const;
  // 便捷: 取字符串成员, 缺失/类型不符返回 fallback。
  std::string GetString(std::string_view key, std::string fallback = {}) const;

 private:
  std::variant<std::monostate, bool, double, std::string, Array, Object> v_;
};

// 解析整个文本(允许首尾空白)。错误消息含字节偏移。
StatusOr<JsonValue> ParseJson(std::string_view text);

}  // namespace momo::engine
