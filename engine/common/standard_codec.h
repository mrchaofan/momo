#pragma once

// StandardCodec: 消息通道的二进制值编码(docs/04 §5), 字节格式与 Flutter StandardMessageCodec 一致,
// JS 侧实现见 framework/packages/momo/src/services/standard_codec.ts(两端必须同步)。
//   0 null | 1 true | 2 false | 3 int32 LE | 4 int64 LE | 6 float64 LE(先按 8 字节对齐)
//   7 UTF-8 string | 8 Uint8 列表 | 9 Int32 列表(4 对齐) | 11 Float64 列表(8 对齐) | 12 list | 13 map
//   长度: <254 一字节;254 + uint16 LE;255 + uint32 LE。对齐以整个消息缓冲的起点为基准。
// StandardMethodCodec(与 Flutter 一致): 请求 = method 值后紧接 args 值;回复 = 信封字节 0 + result,
// 或 1 + code + message + details。
// 线程约定: 纯函数/值类型, 任意线程。

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/status.h"

namespace momo::engine {

class EncodableValue;
using EncodableList = std::vector<EncodableValue>;
using EncodableMap = std::vector<std::pair<EncodableValue, EncodableValue>>;  // 保持插入顺序

class EncodableValue {
 public:
  using Variant = std::variant<std::monostate, bool, int32_t, int64_t, double, std::string,
                               std::vector<uint8_t>, std::vector<int32_t>, std::vector<double>,
                               EncodableList, EncodableMap>;

  EncodableValue() = default;
  EncodableValue(std::nullptr_t) {}                              // NOLINT(implicit)
  EncodableValue(bool v) : v_(v) {}                              // NOLINT(implicit)
  EncodableValue(int32_t v) : v_(v) {}                           // NOLINT(implicit)
  EncodableValue(int64_t v) : v_(v) {}                           // NOLINT(implicit)
  EncodableValue(double v) : v_(v) {}                            // NOLINT(implicit)
  EncodableValue(const char* v) : v_(std::string(v)) {}          // NOLINT(implicit)
  EncodableValue(std::string v) : v_(std::move(v)) {}            // NOLINT(implicit)
  EncodableValue(std::string_view v) : v_(std::string(v)) {}     // NOLINT(implicit)
  EncodableValue(std::vector<uint8_t> v) : v_(std::move(v)) {}   // NOLINT(implicit)
  EncodableValue(std::vector<int32_t> v) : v_(std::move(v)) {}   // NOLINT(implicit)
  EncodableValue(std::vector<double> v) : v_(std::move(v)) {}    // NOLINT(implicit)
  EncodableValue(EncodableList v) : v_(std::move(v)) {}          // NOLINT(implicit)
  EncodableValue(EncodableMap v) : v_(std::move(v)) {}           // NOLINT(implicit)

  const Variant& variant() const { return v_; }

  bool IsNull() const { return std::holds_alternative<std::monostate>(v_); }
  template <typename T>
  bool Is() const { return std::holds_alternative<T>(v_); }
  template <typename T>
  const T* GetIf() const { return std::get_if<T>(&v_); }

  // 宽松读取: 整数(int32/int64)与 double 互通;类型不符返回 nullopt。
  std::optional<int64_t> AsInt() const;
  std::optional<double> AsDouble() const;
  std::optional<std::string> AsString() const;
  // map 中按字符串键查找(非 map 或缺键返回 nullptr)。
  const EncodableValue* Find(std::string_view key) const;

  friend bool operator==(const EncodableValue& a, const EncodableValue& b) { return a.v_ == b.v_; }

 private:
  Variant v_;
};

std::vector<uint8_t> EncodeStandard(const EncodableValue& value);
StatusOr<EncodableValue> DecodeStandard(const uint8_t* data, size_t size);
inline StatusOr<EncodableValue> DecodeStandard(const std::vector<uint8_t>& bytes) {
  return DecodeStandard(bytes.data(), bytes.size());
}

// ---- StandardMethodCodec ----

struct MethodCall {
  std::string method;
  EncodableValue args;
};

std::vector<uint8_t> EncodeMethodCall(const MethodCall& call);
StatusOr<MethodCall> DecodeMethodCall(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> EncodeSuccessEnvelope(const EncodableValue& result);
std::vector<uint8_t> EncodeErrorEnvelope(std::string_view code, std::string_view message);
// 成功 → result;错误回复 → Status(code 与 message 拼进消息)。
StatusOr<EncodableValue> DecodeEnvelope(const std::vector<uint8_t>& bytes);

}  // namespace momo::engine
