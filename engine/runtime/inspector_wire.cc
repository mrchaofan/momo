#include "engine/runtime/inspector_wire.h"

#include <cstring>

namespace momo::engine::runtime::wire {

namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

inline uint32_t RotateLeft(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (AsciiLower(a[i]) != AsciiLower(b[i])) {
      return false;
    }
  }
  return true;
}

void AppendUtf8(std::string* out, uint32_t cp) {
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

}  // namespace

// ---------------------------------------------------------------------------
// SHA-1(FIPS 180-4)

std::array<uint8_t, 20> Sha1(std::string_view data) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

  // 补位: 0x80, 若干 0, 64 位大端比特长度;总长为 64 的倍数。
  std::string message(data);
  const uint64_t bit_length = static_cast<uint64_t>(data.size()) * 8;
  message.push_back(static_cast<char>(0x80));
  while (message.size() % 64 != 56) {
    message.push_back('\0');
  }
  for (int i = 7; i >= 0; --i) {
    message.push_back(static_cast<char>((bit_length >> (i * 8)) & 0xFF));
  }

  for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const auto* p = reinterpret_cast<const uint8_t*>(message.data() + chunk + i * 4);
      w[i] = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f;
      uint32_t k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t temp = RotateLeft(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = RotateLeft(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  std::array<uint8_t, 20> digest{};
  for (int i = 0; i < 5; ++i) {
    digest[i * 4 + 0] = static_cast<uint8_t>(h[i] >> 24);
    digest[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
    digest[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
    digest[i * 4 + 3] = static_cast<uint8_t>(h[i]);
  }
  return digest;
}

std::string Base64Encode(std::string_view bytes) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const uint32_t v = (uint32_t{static_cast<uint8_t>(bytes[i])} << 16) |
                       (uint32_t{static_cast<uint8_t>(bytes[i + 1])} << 8) |
                       static_cast<uint8_t>(bytes[i + 2]);
    out.push_back(kTable[(v >> 18) & 63]);
    out.push_back(kTable[(v >> 12) & 63]);
    out.push_back(kTable[(v >> 6) & 63]);
    out.push_back(kTable[v & 63]);
  }
  const size_t rest = bytes.size() - i;
  if (rest == 1) {
    const uint32_t v = uint32_t{static_cast<uint8_t>(bytes[i])} << 16;
    out.push_back(kTable[(v >> 18) & 63]);
    out.push_back(kTable[(v >> 12) & 63]);
    out += "==";
  } else if (rest == 2) {
    const uint32_t v = (uint32_t{static_cast<uint8_t>(bytes[i])} << 16) |
                       (uint32_t{static_cast<uint8_t>(bytes[i + 1])} << 8);
    out.push_back(kTable[(v >> 18) & 63]);
    out.push_back(kTable[(v >> 12) & 63]);
    out.push_back(kTable[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::string WebSocketAcceptKey(std::string_view client_key) {
  std::string input(Trim(client_key));
  input += kWebSocketGuid;
  const std::array<uint8_t, 20> digest = Sha1(input);
  return Base64Encode(
      std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

// ---------------------------------------------------------------------------
// WebSocket 帧(RFC 6455 §5.2)

std::string EncodeWsFrame(WsOpcode opcode, std::string_view payload, bool fin,
                          const std::array<uint8_t, 4>* mask) {
  std::string out;
  out.reserve(payload.size() + 14);
  out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode)));
  const uint8_t mask_bit = mask != nullptr ? 0x80 : 0x00;
  const uint64_t len = payload.size();
  if (len < 126) {
    out.push_back(static_cast<char>(mask_bit | len));
  } else if (len <= 0xFFFF) {
    out.push_back(static_cast<char>(mask_bit | 126));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
  } else {
    out.push_back(static_cast<char>(mask_bit | 127));
    for (int i = 7; i >= 0; --i) {
      out.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
  }
  if (mask == nullptr) {
    out.append(payload);
    return out;
  }
  out.append(reinterpret_cast<const char*>(mask->data()), 4);
  for (size_t i = 0; i < payload.size(); ++i) {
    out.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ (*mask)[i % 4]));
  }
  return out;
}

std::string EncodeWsClosePayload(uint16_t code, std::string_view reason) {
  std::string out;
  out.push_back(static_cast<char>(code >> 8));
  out.push_back(static_cast<char>(code & 0xFF));
  out.append(reason.substr(0, 123));
  return out;
}

ParseResult ParseWsFrame(std::string_view buffer, size_t max_payload, WsFrame* frame,
                         size_t* consumed) {
  if (buffer.size() < 2) {
    return ParseResult::kIncomplete;
  }
  const auto* p = reinterpret_cast<const uint8_t*>(buffer.data());
  const bool fin = (p[0] & 0x80) != 0;
  if ((p[0] & 0x70) != 0) {
    return ParseResult::kError;  // 未协商扩展, 保留位必须为 0
  }
  const uint8_t op = p[0] & 0x0F;
  if (op != 0x0 && op != 0x1 && op != 0x2 && op != 0x8 && op != 0x9 && op != 0xA) {
    return ParseResult::kError;
  }
  const WsOpcode opcode = static_cast<WsOpcode>(op);
  const bool masked = (p[1] & 0x80) != 0;
  uint64_t len = p[1] & 0x7F;
  size_t pos = 2;
  if (len == 126) {
    if (buffer.size() < 4) {
      return ParseResult::kIncomplete;
    }
    len = (uint64_t{p[2]} << 8) | p[3];
    pos = 4;
  } else if (len == 127) {
    if (buffer.size() < 10) {
      return ParseResult::kIncomplete;
    }
    len = 0;
    for (int i = 0; i < 8; ++i) {
      len = (len << 8) | p[2 + i];
    }
    if ((len >> 63) != 0) {
      return ParseResult::kError;
    }
    pos = 10;
  }
  if (IsControlOpcode(opcode) && (!fin || len > 125)) {
    return ParseResult::kError;
  }
  if (len > max_payload) {
    return ParseResult::kError;
  }
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (buffer.size() < pos + 4) {
      return ParseResult::kIncomplete;
    }
    std::memcpy(mask, p + pos, 4);
    pos += 4;
  }
  if (buffer.size() - pos < len) {
    return ParseResult::kIncomplete;
  }
  frame->fin = fin;
  frame->opcode = opcode;
  frame->masked = masked;
  frame->payload.assign(buffer.data() + pos, static_cast<size_t>(len));
  if (masked) {
    for (size_t i = 0; i < frame->payload.size(); ++i) {
      frame->payload[i] = static_cast<char>(static_cast<uint8_t>(frame->payload[i]) ^ mask[i % 4]);
    }
  }
  *consumed = pos + static_cast<size_t>(len);
  return ParseResult::kOk;
}

// ---------------------------------------------------------------------------
// HTTP

ParseResult ParseHttpRequest(std::string_view buffer, HttpRequest* request, size_t* consumed) {
  const size_t end = buffer.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    return ParseResult::kIncomplete;
  }
  std::string_view head = buffer.substr(0, end);
  *request = HttpRequest();

  size_t line_end = head.find("\r\n");
  std::string_view request_line = head.substr(0, line_end);
  const size_t sp1 = request_line.find(' ');
  const size_t sp2 = sp1 == std::string_view::npos ? sp1 : request_line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp1 == 0 ||
      sp2 == sp1 + 1 || request_line.substr(sp2 + 1).rfind("HTTP/1.", 0) != 0) {
    return ParseResult::kError;
  }
  request->method = std::string(request_line.substr(0, sp1));
  request->target = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
  request->path = request->target.substr(0, request->target.find('?'));

  while (line_end != std::string_view::npos) {
    head.remove_prefix(line_end + 2);
    line_end = head.find("\r\n");
    std::string_view line = head.substr(0, line_end);
    if (line.empty()) {
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
      return ParseResult::kError;
    }
    std::string name;
    for (char c : line.substr(0, colon)) {
      name.push_back(AsciiLower(c));
    }
    const std::string_view value = Trim(line.substr(colon + 1));
    auto [it, inserted] = request->headers.emplace(name, std::string(value));
    if (!inserted) {
      it->second += ", ";
      it->second += value;
    }
  }
  *consumed = end + 4;
  return ParseResult::kOk;
}

bool HeaderHasToken(std::string_view value, std::string_view token) {
  while (!value.empty()) {
    const size_t comma = value.find(',');
    if (EqualsIgnoreCase(Trim(value.substr(0, comma)), token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    value.remove_prefix(comma + 1);
  }
  return false;
}

bool IsLoopbackHost(std::string_view host) {
  host = Trim(host);
  std::string_view name = host;
  if (!host.empty() && host.front() == '[') {
    const size_t close = host.find(']');
    if (close == std::string_view::npos) {
      return false;
    }
    name = host.substr(0, close + 1);
    host.remove_prefix(close + 1);
  } else {
    const size_t colon = host.find(':');
    name = host.substr(0, colon);
    host = colon == std::string_view::npos ? std::string_view() : host.substr(colon);
  }
  if (!host.empty()) {  // ":port"
    if (host.front() != ':' || host.size() == 1) {
      return false;
    }
    for (char c : host.substr(1)) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
  }
  return EqualsIgnoreCase(name, "localhost") || name == "127.0.0.1" || name == "[::1]";
}

std::string JsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// 编码转换

std::u16string Utf8ToUtf16(std::string_view utf8) {
  std::u16string out;
  out.reserve(utf8.size());
  const auto* p = reinterpret_cast<const uint8_t*>(utf8.data());
  const size_t n = utf8.size();
  size_t i = 0;
  while (i < n) {
    const uint8_t b0 = p[i];
    uint32_t cp = 0xFFFD;
    size_t len = 1;
    if (b0 < 0x80) {
      cp = b0;
    } else {
      size_t need = 0;
      uint32_t min = 0;
      if ((b0 & 0xE0) == 0xC0) {
        need = 1;
        cp = b0 & 0x1F;
        min = 0x80;
      } else if ((b0 & 0xF0) == 0xE0) {
        need = 2;
        cp = b0 & 0x0F;
        min = 0x800;
      } else if ((b0 & 0xF8) == 0xF0) {
        need = 3;
        cp = b0 & 0x07;
        min = 0x10000;
      }
      bool valid = need > 0;
      for (size_t k = 1; valid && k <= need; ++k) {
        if (i + k >= n || (p[i + k] & 0xC0) != 0x80) {
          valid = false;
        } else {
          cp = (cp << 6) | (p[i + k] & 0x3F);
        }
      }
      if (valid && (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) {
        valid = false;
      }
      if (valid) {
        len = need + 1;
      } else {
        cp = 0xFFFD;
        len = 1;
      }
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
    i += len;
  }
  return out;
}

std::string Utf16ToUtf8(const char16_t* data, size_t length) {
  std::string out;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    uint32_t cp = data[i];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i + 1 < length && data[i + 1] >= 0xDC00 && data[i + 1] <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (data[i + 1] - 0xDC00);
        ++i;
      } else {
        cp = 0xFFFD;
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    AppendUtf8(&out, cp);
  }
  return out;
}

std::string Latin1ToUtf8(const uint8_t* data, size_t length) {
  std::string out;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    AppendUtf8(&out, data[i]);
  }
  return out;
}

bool IsAscii(std::string_view s) {
  for (char c : s) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      return false;
    }
  }
  return true;
}

}  // namespace momo::engine::runtime::wire
