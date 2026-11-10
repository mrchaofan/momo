#pragma once

// Inspector 线协议的纯函数部分(无 IO、无 V8, 可独立单测):
//   - SHA-1 / Base64(WebSocket 握手 Sec-WebSocket-Accept 用;不引入三方依赖);
//   - RFC 6455 帧编解码(服务端收掩码帧、发不掩码帧;测试客户端反之);
//   - 最小 HTTP/1.1 请求头解析;JSON 字符串转义;
//   - UTF-8 ↔ UTF-16 / Latin-1 → UTF-8(V8 inspector 的 StringView 是 Latin-1 或 UTF-16, 不是 UTF-8)。
// 线程约定: 全部为无状态纯函数, 任意线程可调用。只能被 engine/runtime 的实现与测试包含。

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace momo::engine::runtime::wire {

std::array<uint8_t, 20> Sha1(std::string_view data);
std::string Base64Encode(std::string_view bytes);
// base64(sha1(key + RFC 6455 GUID))
std::string WebSocketAcceptKey(std::string_view client_key);

enum class WsOpcode : uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

inline bool IsControlOpcode(WsOpcode op) { return (static_cast<uint8_t>(op) & 0x8) != 0; }

// 编码一帧。mask 非空时按 RFC 6455 §5.3 掩码(客户端 → 服务端必须掩码)。
std::string EncodeWsFrame(WsOpcode opcode, std::string_view payload, bool fin = true,
                          const std::array<uint8_t, 4>* mask = nullptr);
// close 帧载荷: 2 字节状态码 + 原因。
std::string EncodeWsClosePayload(uint16_t code, std::string_view reason = {});

struct WsFrame {
  bool fin = true;
  WsOpcode opcode = WsOpcode::kText;
  bool masked = false;
  std::string payload;  // 已去掩码
};

enum class ParseResult { kOk, kIncomplete, kError };

// 从 buffer 开头解析一帧。kOk 时 *consumed 为帧的总字节数。
// kError: 保留位非 0、未知 opcode、控制帧分片或载荷 > 125、载荷超过 max_payload。
ParseResult ParseWsFrame(std::string_view buffer, size_t max_payload, WsFrame* frame,
                         size_t* consumed);

struct HttpRequest {
  std::string method;
  std::string target;                          // 原始请求目标(含 query)
  std::string path;                            // 去掉 query 的路径
  std::map<std::string, std::string> headers;  // 键为小写, 值去掉首尾空白
  const std::string* Header(const std::string& lowercase_name) const {
    auto it = headers.find(lowercase_name);
    return it == headers.end() ? nullptr : &it->second;
  }
};

// 解析到 "\r\n\r\n" 为止的请求头。kOk 时 *consumed 为请求头总长度(含空行)。
ParseResult ParseHttpRequest(std::string_view buffer, HttpRequest* request, size_t* consumed);

// 大小写不敏感地判断逗号分隔的头部值是否包含 token(如 Connection: keep-alive, Upgrade)。
bool HeaderHasToken(std::string_view value, std::string_view token);

// 仅允许回环主机名(防 DNS rebinding): localhost / 127.0.0.1 / [::1], 可带 :port。
bool IsLoopbackHost(std::string_view host_header);

std::string JsonEscape(std::string_view utf8);

// 非法 UTF-8 序列替换为 U+FFFD。
std::u16string Utf8ToUtf16(std::string_view utf8);
// 不成对的代理项替换为 U+FFFD。
std::string Utf16ToUtf8(const char16_t* data, size_t length);
std::string Latin1ToUtf8(const uint8_t* data, size_t length);
bool IsAscii(std::string_view s);

}  // namespace momo::engine::runtime::wire
