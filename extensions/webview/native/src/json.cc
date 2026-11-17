#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace momo_webview {

namespace {

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::optional<Json> ParseDocument() {
    std::optional<Json> value = ParseValue(0);
    SkipSpace();
    if (!value || pos_ != text_.size()) return std::nullopt;
    return value;
  }

 private:
  static constexpr int kMaxDepth = 256;

  void SkipSpace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Consume(std::string_view token) {
    if (text_.substr(pos_, token.size()) != token) return false;
    pos_ += token.size();
    return true;
  }

  std::optional<Json> ParseValue(int depth) {
    if (depth > kMaxDepth) return std::nullopt;
    SkipSpace();
    if (pos_ >= text_.size()) return std::nullopt;
    switch (text_[pos_]) {
      case '{': return ParseObject(depth);
      case '[': return ParseArray(depth);
      case '"': {
        std::optional<std::string> s = ParseString();
        if (!s) return std::nullopt;
        return Json(std::move(*s));
      }
      case 't': return Consume("true") ? std::optional<Json>(Json(true)) : std::nullopt;
      case 'f': return Consume("false") ? std::optional<Json>(Json(false)) : std::nullopt;
      case 'n': return Consume("null") ? std::optional<Json>(Json()) : std::nullopt;
      default: return ParseNumber();
    }
  }

  std::optional<Json> ParseObject(int depth) {
    ++pos_;  // {
    Json::Object object;
    SkipSpace();
    if (Consume("}")) return Json(std::move(object));
    while (true) {
      SkipSpace();
      std::optional<std::string> key = ParseString();
      SkipSpace();
      if (!key || !Consume(":")) return std::nullopt;
      std::optional<Json> value = ParseValue(depth + 1);
      if (!value) return std::nullopt;
      object.insert_or_assign(std::move(*key), std::move(*value));
      SkipSpace();
      if (Consume("}")) return Json(std::move(object));
      if (!Consume(",")) return std::nullopt;
    }
  }

  std::optional<Json> ParseArray(int depth) {
    ++pos_;  // [
    Json::Array array;
    SkipSpace();
    if (Consume("]")) return Json(std::move(array));
    while (true) {
      std::optional<Json> value = ParseValue(depth + 1);
      if (!value) return std::nullopt;
      array.push_back(std::move(*value));
      SkipSpace();
      if (Consume("]")) return Json(std::move(array));
      if (!Consume(",")) return std::nullopt;
    }
  }

  std::optional<Json> ParseNumber() {
    const size_t start = pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) return std::nullopt;
    const std::string number(text_.substr(start, pos_ - start));
    char* end = nullptr;
    const double value = std::strtod(number.c_str(), &end);
    if (end != number.c_str() + number.size()) return std::nullopt;
    return Json(value);
  }

  std::optional<uint32_t> ParseHex4() {
    if (pos_ + 4 > text_.size()) return std::nullopt;
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
      else return std::nullopt;
    }
    return value;
  }

  static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  std::optional<std::string> ParseString() {
    if (!Consume("\"")) return std::nullopt;
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= text_.size()) return std::nullopt;
      switch (text_[pos_++]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          std::optional<uint32_t> cp = ParseHex4();
          if (!cp) return std::nullopt;
          if (*cp >= 0xD800 && *cp <= 0xDBFF && Consume("\\u")) {
            std::optional<uint32_t> low = ParseHex4();
            if (!low || *low < 0xDC00 || *low > 0xDFFF) return std::nullopt;
            *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00);
          }
          AppendUtf8(out, *cp);
          break;
        }
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }

  std::string_view text_;
  size_t pos_ = 0;
};

void DumpString(std::string& out, const std::string& s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out += buffer;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

const Json& Null() {
  static const Json null;
  return null;
}

}  // namespace

std::optional<Json> Json::Parse(std::string_view text) { return Parser(text).ParseDocument(); }

std::string Json::Dump() const {
  std::string out;
  DumpTo(out);
  return out;
}

void Json::DumpTo(std::string& out) const {
  if (const bool* b = bool_value()) {
    out += *b ? "true" : "false";
  } else if (const double* d = number()) {
    if (!std::isfinite(*d)) {
      out += "null";
    } else {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.17g", *d);
      out += buffer;
    }
  } else if (const std::string* s = string()) {
    DumpString(out, *s);
  } else if (const Array* a = array()) {
    out += '[';
    for (size_t i = 0; i < a->size(); ++i) {
      if (i > 0) out += ',';
      (*a)[i].DumpTo(out);
    }
    out += ']';
  } else if (const Object* o = object()) {
    out += '{';
    bool first = true;
    for (const auto& [key, value] : *o) {
      if (!first) out += ',';
      first = false;
      DumpString(out, key);
      out += ':';
      value.DumpTo(out);
    }
    out += '}';
  } else {
    out += "null";
  }
}

const Json& Json::operator[](std::string_view key) const {
  const Object* o = object();
  if (o == nullptr) return Null();
  auto it = o->find(key);
  return it != o->end() ? it->second : Null();
}

std::string Json::String(std::string_view key, std::string fallback) const {
  const std::string* s = (*this)[key].string();
  return s != nullptr ? *s : std::move(fallback);
}

std::optional<std::string> Json::OptionalString(std::string_view key) const {
  const std::string* s = (*this)[key].string();
  return s != nullptr ? std::optional<std::string>(*s) : std::nullopt;
}

std::optional<double> Json::Number(std::string_view key) const {
  const double* d = (*this)[key].number();
  return d != nullptr ? std::optional<double>(*d) : std::nullopt;
}

bool Json::Bool(std::string_view key) const {
  const bool* b = (*this)[key].bool_value();
  return b != nullptr && *b;
}

std::vector<std::string> Json::Strings(std::string_view key) const {
  std::vector<std::string> out;
  if (const Array* a = (*this)[key].array()) {
    for (const Json& item : *a) {
      if (const std::string* s = item.string()) out.push_back(*s);
    }
  }
  return out;
}

}  // namespace momo_webview
