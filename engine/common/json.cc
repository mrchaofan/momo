#include "engine/common/json.h"

#include <cstdlib>

namespace momo::engine {

const JsonValue* JsonValue::Get(std::string_view key) const {
  const Object* object = AsObject();
  if (object == nullptr) {
    return nullptr;
  }
  for (const auto& [k, v] : *object) {
    if (k == key) {
      return &v;
    }
  }
  return nullptr;
}

std::string JsonValue::GetString(std::string_view key, std::string fallback) const {
  const JsonValue* v = Get(key);
  const std::string* s = v != nullptr ? v->AsString() : nullptr;
  return s != nullptr ? *s : std::move(fallback);
}

namespace {

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  StatusOr<JsonValue> ParseDocument() {
    JsonValue value;
    if (!ParseValue(&value, 0)) {
      return Error();
    }
    SkipSpace();
    if (pos_ != text_.size()) {
      error_ = "trailing characters";
      return Error();
    }
    return value;
  }

 private:
  Status Error() const {
    return InvalidArgumentError("JSON parse error at byte " + std::to_string(pos_) + ": " +
                                error_);
  }

  void SkipSpace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Fail(const char* message) {
    error_ = message;
    return false;
  }

  bool Literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) {
      return Fail("invalid literal");
    }
    pos_ += word.size();
    return true;
  }

  bool ParseValue(JsonValue* out, int depth) {
    if (depth > 128) {
      return Fail("nesting too deep");
    }
    SkipSpace();
    if (pos_ >= text_.size()) {
      return Fail("unexpected end of input");
    }
    const char c = text_[pos_];
    if (c == '{') return ParseObject(out, depth);
    if (c == '[') return ParseArray(out, depth);
    if (c == '"') {
      std::string s;
      if (!ParseString(&s)) return false;
      *out = JsonValue(std::move(s));
      return true;
    }
    if (c == 't') { *out = JsonValue(true); return Literal("true"); }
    if (c == 'f') { *out = JsonValue(false); return Literal("false"); }
    if (c == 'n') { *out = JsonValue(); return Literal("null"); }
    return ParseNumber(out);
  }

  bool ParseNumber(JsonValue* out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    bool digits = false;
    auto eat_digits = [&] {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
        digits = true;
      }
    };
    eat_digits();
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      eat_digits();
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      eat_digits();
    }
    if (!digits) {
      return Fail("invalid value");
    }
    const std::string number(text_.substr(start, pos_ - start));
    *out = JsonValue(std::strtod(number.c_str(), nullptr));
    return true;
  }

  static void AppendUtf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool Hex4(uint32_t* out) {
    if (pos_ + 4 > text_.size()) return Fail("truncated \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
      else return Fail("invalid \\u escape");
    }
    *out = v;
    return true;
  }

  bool ParseString(std::string* out) {
    ++pos_;  // "
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in string");
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) break;
      const char e = text_[pos_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!Hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && text_.substr(pos_, 2) == "\\u") {
            pos_ += 2;
            uint32_t low = 0;
            if (!Hex4(&low)) return false;
            if (low >= 0xDC00 && low <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
          }
          AppendUtf8(cp, out);
          break;
        }
        default: return Fail("invalid escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseArray(JsonValue* out, int depth) {
    ++pos_;  // [
    JsonValue::Array array;
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      *out = JsonValue(std::move(array));
      return true;
    }
    while (true) {
      JsonValue element;
      if (!ParseValue(&element, depth + 1)) return false;
      array.push_back(std::move(element));
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
      if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; break; }
      return Fail("expected ',' or ']'");
    }
    *out = JsonValue(std::move(array));
    return true;
  }

  bool ParseObject(JsonValue* out, int depth) {
    ++pos_;  // {
    JsonValue::Object object;
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      *out = JsonValue(std::move(object));
      return true;
    }
    while (true) {
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != '"') return Fail("expected string key");
      std::string key;
      if (!ParseString(&key)) return false;
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != ':') return Fail("expected ':'");
      ++pos_;
      JsonValue value;
      if (!ParseValue(&value, depth + 1)) return false;
      object.emplace_back(std::move(key), std::move(value));
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
      if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; break; }
      return Fail("expected ',' or '}'");
    }
    *out = JsonValue(std::move(object));
    return true;
  }

  std::string_view text_;
  size_t pos_ = 0;
  const char* error_ = "";
};

}  // namespace

StatusOr<JsonValue> ParseJson(std::string_view text) { return Parser(text).ParseDocument(); }

}  // namespace momo::engine
