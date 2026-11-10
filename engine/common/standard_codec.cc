#include "engine/common/standard_codec.h"

#include <cstring>

namespace momo::engine {

namespace {

enum Tag : uint8_t {
  kNull = 0,
  kTrue = 1,
  kFalse = 2,
  kInt32 = 3,
  kInt64 = 4,
  kFloat64 = 6,
  kString = 7,
  kUint8List = 8,
  kInt32List = 9,
  kFloat64List = 11,
  kList = 12,
  kMap = 13,
};

class Writer {
 public:
  void Byte(uint8_t b) { out_.push_back(b); }
  void Bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    out_.insert(out_.end(), b, b + n);
  }
  template <typename T>
  void Pod(T v) {  // 小端(目标平台均为小端)
    Bytes(&v, sizeof(T));
  }
  void Align(size_t a) {
    while (out_.size() % a != 0) {
      out_.push_back(0);
    }
  }
  void Size(size_t n) {
    if (n < 254) {
      Byte(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
      Byte(254);
      Pod(static_cast<uint16_t>(n));
    } else {
      Byte(255);
      Pod(static_cast<uint32_t>(n));
    }
  }
  void Value(const EncodableValue& value);
  std::vector<uint8_t> Take() { return std::move(out_); }

 private:
  std::vector<uint8_t> out_;
};

void Writer::Value(const EncodableValue& value) {
  std::visit(
      [this](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          Byte(kNull);
        } else if constexpr (std::is_same_v<T, bool>) {
          Byte(v ? kTrue : kFalse);
        } else if constexpr (std::is_same_v<T, int32_t>) {
          Byte(kInt32);
          Pod(v);
        } else if constexpr (std::is_same_v<T, int64_t>) {
          Byte(kInt64);
          Pod(v);
        } else if constexpr (std::is_same_v<T, double>) {
          Byte(kFloat64);
          Align(8);
          Pod(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
          Byte(kString);
          Size(v.size());
          Bytes(v.data(), v.size());
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
          Byte(kUint8List);
          Size(v.size());
          Bytes(v.data(), v.size());
        } else if constexpr (std::is_same_v<T, std::vector<int32_t>>) {
          Byte(kInt32List);
          Size(v.size());
          Align(4);
          Bytes(v.data(), v.size() * sizeof(int32_t));
        } else if constexpr (std::is_same_v<T, std::vector<double>>) {
          Byte(kFloat64List);
          Size(v.size());
          Align(8);
          Bytes(v.data(), v.size() * sizeof(double));
        } else if constexpr (std::is_same_v<T, EncodableList>) {
          Byte(kList);
          Size(v.size());
          for (const EncodableValue& e : v) {
            Value(e);
          }
        } else if constexpr (std::is_same_v<T, EncodableMap>) {
          Byte(kMap);
          Size(v.size());
          for (const auto& [k, e] : v) {
            Value(k);
            Value(e);
          }
        }
      },
      value.variant());
}

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool ok() const { return ok_; }
  bool AtEnd() const { return pos_ == size_; }

  bool Need(size_t n) {
    if (!ok_ || size_ - pos_ < n) {
      ok_ = false;
      return false;
    }
    return true;
  }
  uint8_t Byte() {
    if (!Need(1)) {
      return 0;
    }
    return data_[pos_++];
  }
  template <typename T>
  T Pod() {
    T v{};
    if (Need(sizeof(T))) {
      std::memcpy(&v, data_ + pos_, sizeof(T));
      pos_ += sizeof(T);
    }
    return v;
  }
  void Align(size_t a) {
    const size_t mod = pos_ % a;
    if (mod != 0 && Need(a - mod)) {
      pos_ += a - mod;
    }
  }
  size_t Size() {
    const uint8_t b = Byte();
    if (b < 254) {
      return b;
    }
    if (b == 254) {
      return Pod<uint16_t>();
    }
    return Pod<uint32_t>();
  }
  EncodableValue Value(int depth);

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

EncodableValue Reader::Value(int depth) {
  if (depth > 256) {  // 防御畸形输入导致的深递归
    ok_ = false;
    return {};
  }
  const uint8_t tag = Byte();
  if (!ok_) {
    return {};
  }
  switch (tag) {
    case kNull: return {};
    case kTrue: return true;
    case kFalse: return false;
    case kInt32: return Pod<int32_t>();
    case kInt64: return Pod<int64_t>();
    case kFloat64:
      Align(8);
      return Pod<double>();
    case kString: {
      const size_t n = Size();
      if (!Need(n)) {
        return {};
      }
      std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
      pos_ += n;
      return s;
    }
    case kUint8List: {
      const size_t n = Size();
      if (!Need(n)) {
        return {};
      }
      std::vector<uint8_t> v(data_ + pos_, data_ + pos_ + n);
      pos_ += n;
      return v;
    }
    case kInt32List: {
      const size_t n = Size();
      Align(4);
      if (n > SIZE_MAX / 4 || !Need(n * 4)) {
        ok_ = false;
        return {};
      }
      std::vector<int32_t> v(n);
      std::memcpy(v.data(), data_ + pos_, n * 4);
      pos_ += n * 4;
      return v;
    }
    case kFloat64List: {
      const size_t n = Size();
      Align(8);
      if (n > SIZE_MAX / 8 || !Need(n * 8)) {
        ok_ = false;
        return {};
      }
      std::vector<double> v(n);
      std::memcpy(v.data(), data_ + pos_, n * 8);
      pos_ += n * 8;
      return v;
    }
    case kList: {
      const size_t n = Size();
      EncodableList list;
      for (size_t i = 0; i < n && ok_; ++i) {
        list.push_back(Value(depth + 1));
      }
      return list;
    }
    case kMap: {
      const size_t n = Size();
      EncodableMap map;
      for (size_t i = 0; i < n && ok_; ++i) {
        EncodableValue k = Value(depth + 1);
        EncodableValue v = Value(depth + 1);
        map.emplace_back(std::move(k), std::move(v));
      }
      return map;
    }
    default:
      ok_ = false;
      return {};
  }
}

}  // namespace

std::optional<int64_t> EncodableValue::AsInt() const {
  if (auto* v = GetIf<int32_t>()) return *v;
  if (auto* v = GetIf<int64_t>()) return *v;
  if (auto* v = GetIf<double>()) return static_cast<int64_t>(*v);
  return std::nullopt;
}

std::optional<double> EncodableValue::AsDouble() const {
  if (auto* v = GetIf<double>()) return *v;
  if (auto* v = GetIf<int32_t>()) return *v;
  if (auto* v = GetIf<int64_t>()) return static_cast<double>(*v);
  return std::nullopt;
}

std::optional<std::string> EncodableValue::AsString() const {
  if (auto* v = GetIf<std::string>()) return *v;
  return std::nullopt;
}

const EncodableValue* EncodableValue::Find(std::string_view key) const {
  const auto* map = GetIf<EncodableMap>();
  if (map == nullptr) {
    return nullptr;
  }
  for (const auto& [k, v] : *map) {
    if (const auto* s = k.GetIf<std::string>(); s != nullptr && *s == key) {
      return &v;
    }
  }
  return nullptr;
}

std::vector<uint8_t> EncodeStandard(const EncodableValue& value) {
  Writer w;
  w.Value(value);
  return w.Take();
}

StatusOr<EncodableValue> DecodeStandard(const uint8_t* data, size_t size) {
  Reader r(data, size);
  EncodableValue value = r.Value(0);
  if (!r.ok() || !r.AtEnd()) {
    return InvalidArgumentError("StandardCodec: malformed message");
  }
  return value;
}

// StandardMethodCodec(与 Flutter 一致): 请求 = method 值紧接 args 值;
// 回复 = 1 字节信封(0 成功 / 1 错误)后接值: 成功 result;错误 code、message、details。
// 对齐仍以整个消息缓冲起点为基准(Writer/Reader 在同一缓冲上连续读写)。
std::vector<uint8_t> EncodeMethodCall(const MethodCall& call) {
  Writer w;
  w.Value(EncodableValue(call.method));
  w.Value(call.args);
  return w.Take();
}

StatusOr<MethodCall> DecodeMethodCall(const std::vector<uint8_t>& bytes) {
  Reader r(bytes.data(), bytes.size());
  EncodableValue method = r.Value(0);
  EncodableValue args = r.AtEnd() ? EncodableValue() : r.Value(0);
  if (!r.ok() || !r.AtEnd() || !method.Is<std::string>()) {
    return InvalidArgumentError("StandardMethodCodec: malformed method call");
  }
  return MethodCall{*method.GetIf<std::string>(), std::move(args)};
}

std::vector<uint8_t> EncodeSuccessEnvelope(const EncodableValue& result) {
  Writer w;
  w.Byte(0);
  w.Value(result);
  return w.Take();
}

std::vector<uint8_t> EncodeErrorEnvelope(std::string_view code, std::string_view message) {
  Writer w;
  w.Byte(1);
  w.Value(EncodableValue(std::string(code)));
  w.Value(EncodableValue(std::string(message)));
  w.Value(EncodableValue());  // details
  return w.Take();
}

StatusOr<EncodableValue> DecodeEnvelope(const std::vector<uint8_t>& bytes) {
  Reader r(bytes.data(), bytes.size());
  const uint8_t kind = r.Byte();
  if (r.ok() && kind == 0) {
    EncodableValue result = r.Value(0);
    if (r.ok() && r.AtEnd()) return result;
  } else if (r.ok() && kind == 1) {
    EncodableValue code = r.Value(0);
    EncodableValue message = r.Value(0);
    if (!r.AtEnd()) r.Value(0);  // details(可省略)
    if (r.ok() && r.AtEnd()) {
      return InternalError(code.AsString().value_or("") + ": " + message.AsString().value_or(""));
    }
  }
  return InvalidArgumentError("StandardMethodCodec: malformed envelope");
}

}  // namespace momo::engine
