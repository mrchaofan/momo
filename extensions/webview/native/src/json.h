#pragma once

// 插件内部的最小 JSON 值: 解析 JS 发来的命令与创建参数, 生成回给 JS 的应答与事件。插件不链接引擎, 所以自带一份。
// 数字统一为 double;字符串为 UTF-8(\uXXXX 含代理对解析为 UTF-8, 输出时只转义控制字符、引号与反斜杠)。
// 值类型, 无共享状态。

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace momo_webview {

class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json, std::less<>>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool value) : value_(value) {}
  Json(int value) : value_(static_cast<double>(value)) {}
  Json(int64_t value) : value_(static_cast<double>(value)) {}
  Json(double value) : value_(value) {}
  Json(const char* value) : value_(std::string(value)) {}
  Json(std::string value) : value_(std::move(value)) {}
  Json(Array value) : value_(std::move(value)) {}
  Json(Object value) : value_(std::move(value)) {}

  // 整个文本是一个 JSON 值(允许首尾空白)时返回它。
  static std::optional<Json> Parse(std::string_view text);
  std::string Dump() const;

  bool is_null() const { return std::holds_alternative<std::monostate>(value_); }
  const bool* bool_value() const { return std::get_if<bool>(&value_); }
  const double* number() const { return std::get_if<double>(&value_); }
  const std::string* string() const { return std::get_if<std::string>(&value_); }
  const Array* array() const { return std::get_if<Array>(&value_); }
  const Object* object() const { return std::get_if<Object>(&value_); }

  // 对象成员;不是对象或没有这个键时为 null。
  const Json& operator[](std::string_view key) const;
  // 成员的便捷读取: 类型不符或缺失时返回 fallback / nullopt。
  std::string String(std::string_view key, std::string fallback = {}) const;
  std::optional<std::string> OptionalString(std::string_view key) const;
  std::optional<double> Number(std::string_view key) const;
  bool Bool(std::string_view key) const;  // 缺失为 false
  // 字符串数组成员中的字符串(其他元素忽略)。
  std::vector<std::string> Strings(std::string_view key) const;

 private:
  void DumpTo(std::string& out) const;

  std::variant<std::monostate, bool, double, std::string, Array, Object> value_;
};

}  // namespace momo_webview
