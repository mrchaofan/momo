#include "engine/runtime/inspector.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "base/blocking_queue.h"
#include "base/testing/scoped_test_event_loop.h"
#include "base/thread.h"
#include "base/waitable_event.h"
#include "engine/runtime/inspector_wire.h"
#include "engine/runtime/js_runtime.h"
#include "gtest/gtest.h"

namespace momo::engine::runtime {
namespace {

// ---------------------------------------------------------------------------
// 协议纯函数

std::string Hex(const std::array<uint8_t, 20>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : digest) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xF]);
  }
  return out;
}

TEST(InspectorWireTest, Sha1AndBase64) {
  EXPECT_EQ(Hex(wire::Sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(Hex(wire::Sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(Hex(wire::Sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  EXPECT_EQ(Hex(wire::Sha1(std::string(1000000, 'a'))),
            "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  EXPECT_EQ(wire::Base64Encode(""), "");
  EXPECT_EQ(wire::Base64Encode("f"), "Zg==");
  EXPECT_EQ(wire::Base64Encode("fo"), "Zm8=");
  EXPECT_EQ(wire::Base64Encode("foo"), "Zm9v");
  EXPECT_EQ(wire::Base64Encode("foobar"), "Zm9vYmFy");
  // RFC 6455 §1.3 的示例。
  EXPECT_EQ(wire::WebSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(InspectorWireTest, FrameRoundTripAllLengthEncodings) {
  const std::array<uint8_t, 4> mask = {0x12, 0x34, 0x56, 0x78};
  for (size_t len : {size_t{0}, size_t{1}, size_t{125}, size_t{126}, size_t{65535},
                     size_t{65536}, size_t{200000}}) {
    std::string payload(len, '\0');
    for (size_t i = 0; i < len; ++i) {
      payload[i] = static_cast<char>(i * 7);
    }
    for (bool masked : {false, true}) {
      const std::string bytes =
          wire::EncodeWsFrame(wire::WsOpcode::kText, payload, true, masked ? &mask : nullptr);
      wire::WsFrame frame;
      size_t consumed = 0;
      // 少一个字节: 不完整。
      EXPECT_EQ(wire::ParseWsFrame(std::string_view(bytes).substr(0, bytes.size() - 1), 1 << 20,
                                   &frame, &consumed),
                wire::ParseResult::kIncomplete);
      ASSERT_EQ(wire::ParseWsFrame(bytes + "trailing", 1 << 20, &frame, &consumed),
                wire::ParseResult::kOk);
      EXPECT_EQ(consumed, bytes.size());
      EXPECT_EQ(frame.masked, masked);
      EXPECT_EQ(frame.opcode, wire::WsOpcode::kText);
      EXPECT_EQ(frame.payload, payload);
    }
  }
}

TEST(InspectorWireTest, FrameErrors) {
  wire::WsFrame frame;
  size_t consumed = 0;
  // 控制帧分片 / 载荷 > 125。
  EXPECT_EQ(wire::ParseWsFrame(wire::EncodeWsFrame(wire::WsOpcode::kPing, "x", false), 100, &frame,
                               &consumed),
            wire::ParseResult::kError);
  EXPECT_EQ(wire::ParseWsFrame(wire::EncodeWsFrame(wire::WsOpcode::kPing, std::string(126, 'x')),
                               1000, &frame, &consumed),
            wire::ParseResult::kError);
  // 超过上限。
  EXPECT_EQ(wire::ParseWsFrame(wire::EncodeWsFrame(wire::WsOpcode::kText, std::string(101, 'x')),
                               100, &frame, &consumed),
            wire::ParseResult::kError);
  // 保留位 / 未知 opcode。
  EXPECT_EQ(wire::ParseWsFrame(std::string("\xC1\x00", 2), 100, &frame, &consumed),
            wire::ParseResult::kError);
  EXPECT_EQ(wire::ParseWsFrame(std::string("\x83\x00", 2), 100, &frame, &consumed),
            wire::ParseResult::kError);
}

TEST(InspectorWireTest, HttpParsingAndHostCheck) {
  wire::HttpRequest request;
  size_t consumed = 0;
  const std::string raw =
      "GET /json/list?x=1 HTTP/1.1\r\nHost: localhost:9229\r\nConnection: keep-alive, Upgrade\r\n"
      "X-A: 1\r\nx-a: 2\r\n\r\nBODY";
  EXPECT_EQ(wire::ParseHttpRequest(raw.substr(0, 20), &request, &consumed),
            wire::ParseResult::kIncomplete);
  ASSERT_EQ(wire::ParseHttpRequest(raw, &request, &consumed), wire::ParseResult::kOk);
  EXPECT_EQ(consumed, raw.size() - 4);
  EXPECT_EQ(request.method, "GET");
  EXPECT_EQ(request.path, "/json/list");
  ASSERT_NE(request.Header("host"), nullptr);
  EXPECT_EQ(*request.Header("host"), "localhost:9229");
  EXPECT_EQ(*request.Header("x-a"), "1, 2");
  EXPECT_TRUE(wire::HeaderHasToken(*request.Header("connection"), "upgrade"));
  EXPECT_EQ(wire::ParseHttpRequest("garbage\r\n\r\n", &request, &consumed),
            wire::ParseResult::kError);

  EXPECT_TRUE(wire::IsLoopbackHost("127.0.0.1:9229"));
  EXPECT_TRUE(wire::IsLoopbackHost("LOCALHOST"));
  EXPECT_TRUE(wire::IsLoopbackHost("[::1]:80"));
  EXPECT_FALSE(wire::IsLoopbackHost("evil.com"));
  EXPECT_FALSE(wire::IsLoopbackHost("localhost.evil.com:9229"));
  EXPECT_FALSE(wire::IsLoopbackHost("127.0.0.1:92x"));
}

TEST(InspectorWireTest, Utf8Utf16) {
  const std::string text = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";  // a é 中 😀
  const std::u16string utf16 = wire::Utf8ToUtf16(text);
  EXPECT_EQ(utf16, u"aé中\U0001F600");
  EXPECT_EQ(wire::Utf16ToUtf8(utf16.data(), utf16.size()), text);
  const uint8_t latin1[] = {'a', 0xE9};
  EXPECT_EQ(wire::Latin1ToUtf8(latin1, 2), "a\xC3\xA9");
  EXPECT_EQ(wire::Utf8ToUtf16("\xFF"), u"�");
  EXPECT_EQ(wire::JsonEscape("a\"b\\\n\x01"), "a\\\"b\\\\\\n\\u0001");
}

// ---------------------------------------------------------------------------
// 测试用最小客户端(阻塞套接字 + 接收超时, 失败不会挂死测试)

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void CloseSocket(SocketHandle s) { closesocket(s); }
struct WinsockScope {
  WinsockScope() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WinsockScope() { WSACleanup(); }
};
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void CloseSocket(SocketHandle s) { close(s); }
struct WinsockScope {};
#endif

class TestSocket {
 public:
  ~TestSocket() { Close(); }

  bool Connect(int port) {
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == kInvalidSocket) {
      return false;
    }
#if defined(_WIN32)
    DWORD timeout_ms = 10000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
#else
    timeval tv{};
    tv.tv_sec = 10;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    return connect(socket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
  }

  bool SendAll(std::string_view data) {
    while (!data.empty()) {
      const auto n = send(socket_, data.data(), static_cast<int>(data.size()), 0);
      if (n <= 0) {
        return false;
      }
      data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
  }

  // 读一些字节追加到 buffer;对端关闭/超时/出错返回 false。
  bool ReadMore() {
    char chunk[16384];
    const auto n = recv(socket_, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(n));
    return true;
  }

  void Close() {
    if (socket_ != kInvalidSocket) {
      CloseSocket(socket_);
      socket_ = kInvalidSocket;
    }
  }

  std::string buffer;

 private:
  SocketHandle socket_ = kInvalidSocket;
};

// GET 一个路径, 读到连接关闭, 返回完整响应(状态行 + 头 + 体)。
std::string HttpGet(int port, const std::string& path, const std::string& host) {
  TestSocket s;
  if (!s.Connect(port) ||
      !s.SendAll("GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nAccept: */*\r\n\r\n")) {
    return {};
  }
  while (s.ReadMore()) {
  }
  return s.buffer;
}

class WsClient {
 public:
  bool Connect(int port, const std::string& path) {
    if (!socket_.Connect(port)) {
      return false;
    }
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    if (!socket_.SendAll("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                         "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n")) {
      return false;
    }
    size_t end;
    while ((end = socket_.buffer.find("\r\n\r\n")) == std::string::npos) {
      if (!socket_.ReadMore()) {
        return false;
      }
    }
    const std::string head = socket_.buffer.substr(0, end);
    socket_.buffer.erase(0, end + 4);
    return head.rfind("HTTP/1.1 101", 0) == 0 &&
           head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos;
  }

  bool SendFrame(wire::WsOpcode opcode, std::string_view payload, bool fin = true) {
    return socket_.SendAll(wire::EncodeWsFrame(opcode, payload, fin, &kMask));
  }
  bool SendText(std::string_view text) { return SendFrame(wire::WsOpcode::kText, text); }

  // 读下一帧(服务端帧不掩码)。
  std::optional<wire::WsFrame> ReadFrame() {
    for (;;) {
      wire::WsFrame frame;
      size_t consumed = 0;
      const wire::ParseResult r =
          wire::ParseWsFrame(socket_.buffer, size_t{1} << 30, &frame, &consumed);
      if (r == wire::ParseResult::kOk) {
        socket_.buffer.erase(0, consumed);
        return frame;
      }
      if (r == wire::ParseResult::kError || !socket_.ReadMore()) {
        return std::nullopt;
      }
    }
  }

  // 读文本消息, 直到包含 needle;返回该消息。
  std::optional<std::string> WaitFor(const std::string& needle) {
    while (std::optional<wire::WsFrame> frame = ReadFrame()) {
      if (frame->opcode == wire::WsOpcode::kText) {
        transcript += frame->payload + "\n";
        if (frame->payload.find(needle) != std::string::npos) {
          return frame->payload;
        }
      }
      if (frame->opcode == wire::WsOpcode::kClose) {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  void Close() { socket_.Close(); }

  std::string transcript;  // WaitFor 读到的全部文本消息

 private:
  static constexpr std::array<uint8_t, 4> kMask = {0xA1, 0xB2, 0xC3, 0xD4};
  TestSocket socket_;
};

std::string PathOf(const std::string& ws_url) {
  return ws_url.substr(ws_url.find('/', std::string("ws://").size()));
}

int FindFreePort() {
  SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
  CloseSocket(s);
  return ntohs(addr.sin_port);
}

// ---------------------------------------------------------------------------
// 端到端: 测试线程即 ui 线程(JsRuntime + Inspector), DevTools 客户端跑在另一个 momo::Thread 上。

class InspectorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto runtime = JsRuntime::Create({});
    ASSERT_TRUE(runtime.ok()) << runtime.status();
    runtime_ = std::move(runtime).value();
  }
  void TearDown() override {
    inspector_.reset();
    runtime_.reset();
  }

  // 网络用例: 系统分配端口。沙箱禁止绑定本地端口(EPERM)时跳过 —— 调用方须用
  // ASSERT_NO_FATAL_FAILURE 包裹并在 IsSkipped() 时返回。
  void StartInspector(Inspector::Options options = {}) {
    options.port = 0;
    auto inspector = Inspector::Start(*runtime_, options);
    if (!inspector.ok() &&
        inspector.status().message().find("Operation not permitted") != std::string::npos) {
      GTEST_SKIP() << "binding a local port is not permitted here: " << inspector.status();
    }
    ASSERT_TRUE(inspector.ok()) << inspector.status();
    inspector_ = std::move(inspector).value();
    ASSERT_GT(inspector_->port(), 0);
  }

  // 进程内会话用例: 不启动网络服务。
  void StartLocalInspector() {
    Inspector::Options options;
    options.port = -1;
    auto inspector = Inspector::Start(*runtime_, options);
    ASSERT_TRUE(inspector.ok()) << inspector.status();
    inspector_ = std::move(inspector).value();
    EXPECT_EQ(inspector_->port(), -1);
    EXPECT_EQ(inspector_->websocket_url(), "");
  }

  // 在 client 线程上运行 fn。
  void RunClient(std::function<void()> fn) {
    client_.GetTaskRunner()->PostTask([fn = std::move(fn), this] {
      fn();
      client_done_.Signal();
    });
  }

  // 以 ui 线程身份驱动 EventLoop, 直到 event 被触发(或超时失败)。
  bool PumpUntil(WaitableEvent& event) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!event.IsSignaled()) {
      loop_.RunUntilIdle();
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    loop_.RunUntilIdle();
    return true;
  }

  WinsockScope winsock_;
  momo::testing::ScopedTestEventLoop loop_;
  std::unique_ptr<JsRuntime> runtime_;
  std::unique_ptr<Inspector> inspector_;
  Thread client_{"inspector.test.client"};
  WaitableEvent client_done_;
};

TEST_F(InspectorTest, JsonListAndVersionEndpoints) {
  ASSERT_NO_FATAL_FAILURE(
      StartInspector({.target_title = "My \"App\"", .target_url = "file:///app/main.js"}));
  if (IsSkipped()) {
    return;
  }
  const int port = inspector_->port();
  const std::string host = "127.0.0.1:" + std::to_string(port);

  const std::string list = HttpGet(port, "/json/list", host);
  ASSERT_EQ(list.rfind("HTTP/1.1 200", 0), 0u) << list;
  EXPECT_NE(list.find("application/json"), std::string::npos);
  EXPECT_NE(list.find("\"webSocketDebuggerUrl\": \"" + inspector_->websocket_url() + "\""),
            std::string::npos)
      << list;
  EXPECT_NE(list.find("\"devtoolsFrontendUrl\": \"devtools://devtools/bundled/js_app.html?"
                      "experiments=true&v8only=true&ws=" + host + PathOf(inspector_->websocket_url())),
            std::string::npos)
      << list;
  EXPECT_EQ(inspector_->devtools_frontend_url(),
            "devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=" + host +
                PathOf(inspector_->websocket_url()));
  EXPECT_NE(list.find("\"title\": \"My \\\"App\\\"\""), std::string::npos) << list;
  EXPECT_NE(list.find("\"url\": \"file:///app/main.js\""), std::string::npos) << list;

  EXPECT_EQ(HttpGet(port, "/json", "localhost:" + std::to_string(port)).rfind("HTTP/1.1 200", 0),
            0u);
  const std::string version = HttpGet(port, "/json/version", host);
  EXPECT_NE(version.find("\"Protocol-Version\": \"1.1\""), std::string::npos) << version;
  EXPECT_NE(version.find("Momo/V8-"), std::string::npos) << version;

  EXPECT_EQ(HttpGet(port, "/nope", host).rfind("HTTP/1.1 404", 0), 0u);
  // DNS rebinding 防护: 非回环 Host 拒绝。
  EXPECT_EQ(HttpGet(port, "/json/list", "evil.example:" + std::to_string(port))
                .rfind("HTTP/1.1 403", 0),
            0u);
}

TEST_F(InspectorTest, RuntimeEvaluateOverWebSocket) {
  ASSERT_NO_FATAL_FAILURE(StartInspector());
  if (IsSkipped()) {
    return;
  }
  const int port = inspector_->port();
  const std::string path = PathOf(inspector_->websocket_url());
  std::string result;
  std::string unicode_result;
  std::string fragmented_result;
  bool got_pong = false;
  RunClient([&] {
    WsClient ws;
    ASSERT_TRUE(ws.Connect(port, path));
    ASSERT_TRUE(ws.SendText(R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"1+2"}})"));
    result = ws.WaitFor(R"("id":1)").value_or("");
    // 非 ASCII 往返(UTF-8 ↔ V8 的 UTF-16)。
    ASSERT_TRUE(ws.SendText(
        "{\"id\":2,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"'\xE4\xB8\xAD' + "
        "'\xE6\x96\x87'\"}}"));
    unicode_result = ws.WaitFor(R"("id":2)").value_or("");
    // 分片消息, 中间插一个 ping(控制帧可插在分片之间)。
    const std::string msg = R"({"id":3,"method":"Runtime.evaluate","params":{"expression":"6*7"}})";
    ASSERT_TRUE(ws.SendFrame(wire::WsOpcode::kText, msg.substr(0, 10), false));
    ASSERT_TRUE(ws.SendFrame(wire::WsOpcode::kContinuation, msg.substr(10, 20), false));
    ASSERT_TRUE(ws.SendFrame(wire::WsOpcode::kPing, "hb"));
    ASSERT_TRUE(ws.SendFrame(wire::WsOpcode::kContinuation, msg.substr(30), true));
    for (;;) {
      std::optional<wire::WsFrame> frame = ws.ReadFrame();
      ASSERT_TRUE(frame.has_value());
      if (frame->opcode == wire::WsOpcode::kPong) {
        got_pong = frame->payload == "hb";
      } else if (frame->payload.find(R"("id":3)") != std::string::npos) {
        fragmented_result = frame->payload;
        break;
      }
    }
    ASSERT_TRUE(ws.SendFrame(wire::WsOpcode::kClose, wire::EncodeWsClosePayload(1000)));
    std::optional<wire::WsFrame> close = ws.ReadFrame();
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->opcode, wire::WsOpcode::kClose);
  });
  ASSERT_TRUE(PumpUntil(client_done_));
  EXPECT_NE(result.find(R"("type":"number","value":3)"), std::string::npos) << result;
  EXPECT_NE(unicode_result.find(R"("value":"\u4e2d\u6587")"), std::string::npos)
      << unicode_result;
  EXPECT_NE(fragmented_result.find(R"("value":42)"), std::string::npos) << fragmented_result;
  EXPECT_TRUE(got_pong);
}

TEST_F(InspectorTest, DebuggerStatementPausesUntilResume) {
  ASSERT_NO_FATAL_FAILURE(StartInspector());
  if (IsSkipped()) {
    return;
  }
  const int port = inspector_->port();
  const std::string path = PathOf(inspector_->websocket_url());
  WaitableEvent enabled;
  std::string paused;
  std::string console_message;
  std::string transcript;
  bool resumed = false;
  RunClient([&] {
    WsClient ws;
    ASSERT_TRUE(ws.Connect(port, path));
    ASSERT_TRUE(ws.SendText(R"({"id":1,"method":"Runtime.enable"})"));
    ASSERT_TRUE(ws.WaitFor(R"("id":1)").has_value());
    ASSERT_TRUE(ws.SendText(R"({"id":2,"method":"Debugger.enable"})"));
    ASSERT_TRUE(ws.WaitFor(R"("id":2)").has_value());
    enabled.Signal();
    console_message = ws.WaitFor("Runtime.consoleAPICalled").value_or("");
    paused = ws.WaitFor("Debugger.paused").value_or("");
    ASSERT_TRUE(ws.SendText(R"({"id":3,"method":"Debugger.resume"})"));
    resumed = ws.WaitFor(R"("id":3)").has_value();
    transcript = ws.transcript;
  });
  ASSERT_TRUE(PumpUntil(enabled));
  // ui 线程在 debugger 语句处阻塞(runMessageLoopOnPause), 直到客户端发 Debugger.resume。
  auto value = runtime_->EvalToString(
      "console.log('hello from js');\n"
      "globalThis.x = 1;\n"
      "debugger;\n"
      "x = 2;\n"
      "x",
      "pause_test.js");
  ASSERT_TRUE(value.ok()) << value.status();
  EXPECT_EQ(*value, "2");
  ASSERT_TRUE(PumpUntil(client_done_));
  EXPECT_NE(console_message.find("hello from js"), std::string::npos) << console_message;
  EXPECT_NE(paused.find(R"("reason":"other")"), std::string::npos) << paused;
  // 脚本 url 出现在 Debugger.scriptParsed 中。
  EXPECT_NE(transcript.find(R"("url":"pause_test.js")"), std::string::npos) << transcript;
  EXPECT_TRUE(resumed);
}

TEST_F(InspectorTest, DisconnectWhilePausedResumesExecution) {
  ASSERT_NO_FATAL_FAILURE(StartInspector());
  if (IsSkipped()) {
    return;
  }
  const int port = inspector_->port();
  const std::string path = PathOf(inspector_->websocket_url());
  WaitableEvent enabled;
  bool paused = false;
  RunClient([&] {
    WsClient ws;
    ASSERT_TRUE(ws.Connect(port, path));
    ASSERT_TRUE(ws.SendText(R"({"id":1,"method":"Debugger.enable"})"));
    ASSERT_TRUE(ws.WaitFor(R"("id":1)").has_value());
    enabled.Signal();
    paused = ws.WaitFor("Debugger.paused").has_value();
    ws.Close();  // 不 resume 直接断开
  });
  ASSERT_TRUE(PumpUntil(enabled));
  auto value = runtime_->EvalToString("debugger; 'done'");
  ASSERT_TRUE(value.ok()) << value.status();
  EXPECT_EQ(*value, "done");
  ASSERT_TRUE(PumpUntil(client_done_));
  EXPECT_TRUE(paused);
  EXPECT_EQ(inspector_->session_count(), 0);
}

TEST_F(InspectorTest, WaitForDebuggerBlocksUntilRunIfWaiting) {
  {
    // 探测能否绑定本地端口(沙箱)。
    ASSERT_NO_FATAL_FAILURE(StartInspector());
    if (IsSkipped()) {
      return;
    }
    inspector_.reset();
  }
  const int port = FindFreePort();
  bool got_response = false;
  RunClient([&] {
    // Start 在 ui 线程上阻塞;服务器就绪前连接会失败, 重试。
    std::string list;
    for (int i = 0; i < 2000 && list.find("webSocketDebuggerUrl") == std::string::npos; ++i) {
      list = HttpGet(port, "/json/list", "127.0.0.1:" + std::to_string(port));
      if (list.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    const size_t begin = list.find("ws://");
    ASSERT_NE(begin, std::string::npos) << list;
    const std::string url = list.substr(begin, list.find('"', begin) - begin);
    WsClient ws;
    ASSERT_TRUE(ws.Connect(port, PathOf(url)));
    ASSERT_TRUE(ws.SendText(R"({"id":1,"method":"Runtime.runIfWaitingForDebugger"})"));
    got_response = ws.WaitFor(R"("id":1)").has_value();
  });
  Inspector::Options options;
  options.port = port;
  options.wait_for_debugger = true;
  auto inspector = Inspector::Start(*runtime_, options);  // 阻塞到客户端发 runIfWaitingForDebugger
  ASSERT_TRUE(inspector.ok()) << inspector.status();
  inspector_ = std::move(inspector).value();
  EXPECT_EQ(inspector_->session_count(), 1);
  ASSERT_TRUE(PumpUntil(client_done_));
  EXPECT_TRUE(got_response);
}

TEST_F(InspectorTest, PortInUseFails) {
  ASSERT_NO_FATAL_FAILURE(StartInspector());
  if (IsSkipped()) {
    return;
  }
  Inspector::Options options;
  options.port = inspector_->port();
  auto second = Inspector::Start(*runtime_, options);
  ASSERT_FALSE(second.ok());
  EXPECT_NE(second.status().message().find("bind"), std::string::npos) << second.status();
}

// ---------------------------------------------------------------------------
// 进程内会话: 不经网络, 覆盖同一套 V8 分发/暂停泵逻辑(沙箱内也能跑)。
// 消息在 ui 线程经 on_message 收到, 转入队列供 client 线程阻塞读取。

class LocalPeer {
 public:
  void Connect(Inspector& inspector) {
    std::shared_ptr<BlockingQueue<std::string>> received = received_;
    session_ = inspector.ConnectLocal(
        [received](const std::string& message) { received->Push(message); });
  }
  void Send(std::string message) { session_->Send(std::move(message)); }
  void Disconnect() { session_.reset(); }

  std::string transcript;  // WaitFor 读到的全部消息(仅 client 线程访问)

  std::optional<std::string> WaitFor(const std::string& needle) {
    while (std::optional<std::string> message = received_->PopFor(std::chrono::seconds(10))) {
      transcript += *message + "\n";
      if (message->find(needle) != std::string::npos) {
        return message;
      }
    }
    return std::nullopt;
  }

 private:
  std::shared_ptr<BlockingQueue<std::string>> received_ =
      std::make_shared<BlockingQueue<std::string>>();
  std::unique_ptr<Inspector::LocalSession> session_;
};

TEST_F(InspectorTest, LocalSessionEvaluate) {
  ASSERT_NO_FATAL_FAILURE(StartLocalInspector());
  LocalPeer peer;
  peer.Connect(*inspector_);
  EXPECT_EQ(inspector_->session_count(), 1);
  std::string result;
  std::string unicode_result;
  RunClient([&] {
    peer.Send(R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"1+2"}})");
    result = peer.WaitFor(R"("id":1)").value_or("");
    peer.Send(
        "{\"id\":2,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"'\xE4\xB8\xAD' + "
        "'\xE6\x96\x87'\"}}");
    unicode_result = peer.WaitFor(R"("id":2)").value_or("");
  });
  ASSERT_TRUE(PumpUntil(client_done_));
  EXPECT_NE(result.find(R"("type":"number","value":3)"), std::string::npos) << result;
  EXPECT_NE(unicode_result.find(R"("value":"\u4e2d\u6587")"), std::string::npos)
      << unicode_result;
  peer.Disconnect();
  loop_.RunUntilIdle();
  EXPECT_EQ(inspector_->session_count(), 0);
}

TEST_F(InspectorTest, LocalSessionPauseResumeAndConsole) {
  ASSERT_NO_FATAL_FAILURE(StartLocalInspector());
  LocalPeer peer;
  peer.Connect(*inspector_);
  WaitableEvent enabled;
  std::string console_message;
  std::string paused;
  std::string evaluated_while_paused;
  bool resumed = false;
  RunClient([&] {
    peer.Send(R"({"id":1,"method":"Runtime.enable"})");
    ASSERT_TRUE(peer.WaitFor(R"("id":1)").has_value());
    peer.Send(R"({"id":2,"method":"Debugger.enable"})");
    ASSERT_TRUE(peer.WaitFor(R"("id":2)").has_value());
    enabled.Signal();
    console_message = peer.WaitFor("Runtime.consoleAPICalled").value_or("");
    paused = peer.WaitFor("Debugger.paused").value_or("");
    // 暂停期间照常处理消息: 在暂停帧上求值。
    const size_t at = paused.find(R"("callFrameId":")");
    ASSERT_NE(at, std::string::npos) << paused;
    const size_t begin = at + std::string(R"("callFrameId":")").size();
    const std::string frame_id = paused.substr(begin, paused.find('"', begin) - begin);
    peer.Send(R"({"id":3,"method":"Debugger.evaluateOnCallFrame","params":{"callFrameId":")" +
              frame_id + R"(","expression":"x * 10"}})");
    evaluated_while_paused = peer.WaitFor(R"("id":3)").value_or("");
    peer.Send(R"({"id":4,"method":"Debugger.resume"})");
    resumed = peer.WaitFor(R"("id":4)").has_value();
  });
  ASSERT_TRUE(PumpUntil(enabled));
  auto value = runtime_->EvalToString(
      "console.log('hello', 'from js');\n"
      "globalThis.x = 1;\n"
      "debugger;\n"
      "x = 2;\n"
      "x",
      "local_pause_test.js");
  ASSERT_TRUE(value.ok()) << value.status();
  EXPECT_EQ(*value, "2");
  ASSERT_TRUE(PumpUntil(client_done_));
  EXPECT_NE(console_message.find(R"("type":"log")"), std::string::npos) << console_message;
  EXPECT_NE(console_message.find("from js"), std::string::npos) << console_message;
  EXPECT_NE(peer.transcript.find(R"("url":"local_pause_test.js")"), std::string::npos)
      << peer.transcript;
  EXPECT_NE(evaluated_while_paused.find(R"("value":10)"), std::string::npos)
      << evaluated_while_paused;
  EXPECT_TRUE(resumed);
}

TEST_F(InspectorTest, LocalSessionPauseInsideEvaluateAndDisconnect) {
  ASSERT_NO_FATAL_FAILURE(StartLocalInspector());
  LocalPeer peer;
  peer.Connect(*inspector_);
  bool paused = false;
  RunClient([&] {
    peer.Send(R"({"id":1,"method":"Debugger.enable"})");
    ASSERT_TRUE(peer.WaitFor(R"("id":1)").has_value());
    // 暂停发生在本会话的 dispatchProtocolMessage 栈内;此时断开不能就地销毁会话。
    peer.Send(R"({"id":2,"method":"Runtime.evaluate","params":{"expression":"debugger; 7"}})");
    paused = peer.WaitFor("Debugger.paused").has_value();
    peer.Disconnect();
  });
  ASSERT_TRUE(PumpUntil(client_done_));
  EXPECT_TRUE(paused);
  EXPECT_EQ(inspector_->session_count(), 0);
  // 会话断开后执行已继续, JS 仍可用。
  auto value = runtime_->EvalToString("debugger; 'alive'");
  ASSERT_TRUE(value.ok()) << value.status();
  EXPECT_EQ(*value, "alive");
}

TEST_F(InspectorTest, LocalSessionDisconnectWhilePausedResumes) {
  ASSERT_NO_FATAL_FAILURE(StartLocalInspector());
  LocalPeer peer;
  peer.Connect(*inspector_);
  WaitableEvent enabled;
  bool paused = false;
  RunClient([&] {
    peer.Send(R"({"id":1,"method":"Debugger.enable"})");
    ASSERT_TRUE(peer.WaitFor(R"("id":1)").has_value());
    enabled.Signal();
    paused = peer.WaitFor("Debugger.paused").has_value();
    peer.Disconnect();
  });
  ASSERT_TRUE(PumpUntil(enabled));
  auto value = runtime_->EvalToString("debugger; 'done'");
  ASSERT_TRUE(value.ok()) << value.status();
  EXPECT_EQ(*value, "done");
  ASSERT_TRUE(PumpUntil(client_done_));
  EXPECT_TRUE(paused);
  EXPECT_EQ(inspector_->session_count(), 0);
}

TEST_F(InspectorTest, LocalSessionWaitForDebugger) {
  ASSERT_NO_FATAL_FAILURE(StartLocalInspector());
  LocalPeer peer;
  peer.Connect(*inspector_);
  bool got_response = false;
  RunClient([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    peer.Send(R"({"id":1,"method":"Runtime.enable"})");
    peer.Send(R"({"id":2,"method":"Runtime.runIfWaitingForDebugger"})");
    got_response = peer.WaitFor(R"("id":2)").has_value();
  });
  inspector_->WaitForDebugger();  // 阻塞到 id 2 被分发
  ASSERT_TRUE(PumpUntil(client_done_));
  EXPECT_TRUE(got_response);
}

TEST_F(InspectorTest, ConsoleStillLogsWithoutInspector) {
  // 自定义 console 包装了 V8 内建 console: 未挂 Inspector 时转发是空操作, 不影响返回值。
  auto value = runtime_->EvalToString(
      "console.log('a', {b: 1}); console.warn('w'); typeof console.table + ',' + typeof console.error");
  ASSERT_TRUE(value.ok()) << value.status();
  EXPECT_EQ(*value, "function,function");
}

}  // namespace
}  // namespace momo::engine::runtime
