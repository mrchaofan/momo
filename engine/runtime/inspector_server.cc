#include "engine/runtime/inspector_server.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <vector>

#include "base/logging.h"
#include "base/thread.h"
#include "engine/runtime/inspector_wire.h"

namespace momo::engine::runtime {

namespace {

// ---------------------------------------------------------------------------
// 套接字可移植层: POSIX(macOS/Linux)与 Winsock。

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
using PollFd = WSAPOLLFD;

int PollSockets(PollFd* fds, size_t count, int timeout_ms) {
  return WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
}
void CloseSocket(SocketHandle s) { closesocket(s); }
bool SetNonBlocking(SocketHandle s) {
  u_long one = 1;
  return ioctlsocket(s, FIONBIO, &one) == 0;
}
int LastSocketError() { return WSAGetLastError(); }
bool IsWouldBlock(int err) { return err == WSAEWOULDBLOCK; }
bool IsInterrupted(int err) { return err == WSAEINTR; }
long SendBytes(SocketHandle s, const char* data, size_t size) {
  return send(s, data, static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
}
long RecvBytes(SocketHandle s, char* data, size_t size) {
  return recv(s, data, static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
}
void ConfigureAcceptedSocket(SocketHandle) {}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
using PollFd = pollfd;

int PollSockets(PollFd* fds, size_t count, int timeout_ms) {
  return poll(fds, static_cast<nfds_t>(count), timeout_ms);
}
void CloseSocket(SocketHandle s) { close(s); }
bool SetNonBlocking(SocketHandle s) {
  const int flags = fcntl(s, F_GETFL, 0);
  return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0 &&
         fcntl(s, F_SETFD, FD_CLOEXEC) == 0;
}
int LastSocketError() { return errno; }
bool IsWouldBlock(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
bool IsInterrupted(int err) { return err == EINTR; }
long SendBytes(SocketHandle s, const char* data, size_t size) {
#if defined(MSG_NOSIGNAL)
  constexpr int kFlags = MSG_NOSIGNAL;  // Linux: 对端关闭时不产生 SIGPIPE
#else
  constexpr int kFlags = 0;  // macOS: 已在套接字上设置 SO_NOSIGPIPE
#endif
  return static_cast<long>(send(s, data, size, kFlags));
}
long RecvBytes(SocketHandle s, char* data, size_t size) {
  return static_cast<long>(recv(s, data, size, 0));
}
void ConfigureAcceptedSocket(SocketHandle s) {
#if defined(SO_NOSIGPIPE)
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)s;
#endif
}
#endif

std::string SocketErrorString(int err) {
#if defined(_WIN32)
  return "winsock error " + std::to_string(err);
#else
  return std::string(std::strerror(err)) + " (" + std::to_string(err) + ")";
#endif
}

constexpr size_t kMaxHttpHeaderSize = 64 * 1024;
constexpr size_t kMaxMessageSize = 64 * 1024 * 1024;
constexpr size_t kReadChunk = 64 * 1024;

// WebSocket 关闭码(RFC 6455 §7.4.1)
constexpr uint16_t kCloseNormal = 1000;
constexpr uint16_t kCloseGoingAway = 1001;
constexpr uint16_t kCloseProtocolError = 1002;
constexpr uint16_t kCloseTooBig = 1009;

std::string HttpResponse(int status, std::string_view reason, std::string_view content_type,
                         std::string_view body) {
  std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n";
  out += "Content-Type: " + std::string(content_type) + "\r\n";
  out += "Cache-Control: no-cache\r\n";
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  out += "Connection: close\r\n\r\n";
  out.append(body);
  return out;
}

std::string TextResponse(int status, std::string_view reason) {
  return HttpResponse(status, reason, "text/plain; charset=UTF-8", std::string(reason) + "\n");
}

}  // namespace

// ---------------------------------------------------------------------------
// IoState: 套接字与连接表。Start 期间在调用线程初始化, 之后仅 IO 线程访问;
// 析构在 IO 线程 join 之后。例外: wake_send 在 Wake() 中被任意线程只读使用(写入只发生在启动前)。

struct Connection {
  enum class State { kHttp, kWebSocket };

  int id = 0;
  SocketHandle socket = kInvalidSocket;
  State state = State::kHttp;
  std::string in;
  std::string out;
  bool close_after_write = false;  // 写完 out 后关闭
  bool dead = false;               // 待回收
  bool close_sent = false;         // 已发送 close 帧
  bool announced = false;          // 已推送 kConnected(回收时需推送 kDisconnected)
  bool in_message = false;         // 分片消息进行中
  std::string message;
};

struct InspectorServer::IoState {
  SocketHandle listen_socket = kInvalidSocket;
#if defined(_WIN32)
  // Windows 没有可 poll 的管道: 用绑定在回环上的 UDP 套接字自发自收作为唤醒通道。
  SocketHandle wake_socket = kInvalidSocket;
  sockaddr_in wake_addr{};
  bool wsa_started = false;
#else
  int wake_pipe[2] = {-1, -1};  // [0] 读端(IO 线程 poll), [1] 写端(任意线程)
#endif
  std::map<int, std::unique_ptr<Connection>> connections;
  int next_id = 1;

  ~IoState() {
    for (auto& [id, conn] : connections) {
      CloseSocket(conn->socket);
    }
    if (listen_socket != kInvalidSocket) {
      CloseSocket(listen_socket);
    }
#if defined(_WIN32)
    if (wake_socket != kInvalidSocket) {
      CloseSocket(wake_socket);
    }
    if (wsa_started) {
      WSACleanup();
    }
#else
    for (int fd : wake_pipe) {
      if (fd >= 0) {
        close(fd);
      }
    }
#endif
  }
};

// ---------------------------------------------------------------------------

StatusOr<std::unique_ptr<InspectorServer>> InspectorServer::Start(
    Options options, std::shared_ptr<EventQueue> inbound,
    std::shared_ptr<TaskRunner> notify_runner, std::function<void()> notify) {
  MOMO_CHECK(inbound != nullptr);
  MOMO_CHECK(notify_runner != nullptr || !notify);
  std::function<void()> post_notify;
  if (notify) {
    post_notify = [runner = std::move(notify_runner), notify = std::move(notify)] {
      runner->PostTask(notify);
    };
  }
  std::unique_ptr<InspectorServer> server(
      new InspectorServer(std::move(options), std::move(inbound), std::move(post_notify)));
  MOMO_RETURN_IF_ERROR(server->Listen());
  Thread::Options thread_options;
  thread_options.stack_size = 0;  // 系统默认即可
  server->thread_ = std::make_unique<Thread>("momo.inspector.io", thread_options);
  InspectorServer* raw = server.get();
  server->thread_->GetTaskRunner()->PostTask([raw] { raw->IoMain(); });
  return server;
}

InspectorServer::InspectorServer(Options options, std::shared_ptr<EventQueue> inbound,
                                 std::function<void()> post_notify)
    : options_(std::move(options)),
      notify_(std::move(post_notify)),
      inbound_(std::move(inbound)),
      io_(std::make_unique<IoState>()) {}

InspectorServer::~InspectorServer() {
  stop_.store(true, std::memory_order_release);
  if (thread_ != nullptr) {
    Wake();
    thread_->Join();  // IoMain 看到 stop_ 后退出;之后 io_ 仅本线程访问
    thread_.reset();
  }
  io_.reset();
  outbound_.Close();
}

Status InspectorServer::Listen() {
  IoState& io = *io_;
#if defined(_WIN32)
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return UnavailableError("inspector: WSAStartup failed");
  }
  io.wsa_started = true;
#endif
  io.listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (io.listen_socket == kInvalidSocket) {
    return UnavailableError("inspector: socket() failed: " + SocketErrorString(LastSocketError()));
  }
#if defined(_WIN32)
  // Windows 的 SO_REUSEADDR 允许端口被劫持, 改用独占。
  BOOL exclusive = TRUE;
  setsockopt(io.listen_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
  int reuse = 1;
  setsockopt(io.listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 只监听 127.0.0.1
  addr.sin_port = htons(static_cast<uint16_t>(options_.port));
  if (bind(io.listen_socket, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    return UnavailableError("inspector: cannot bind 127.0.0.1:" + std::to_string(options_.port) +
                            ": " + SocketErrorString(LastSocketError()));
  }
  if (listen(io.listen_socket, 16) != 0) {
    return UnavailableError("inspector: listen() failed: " +
                            SocketErrorString(LastSocketError()));
  }
  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  if (getsockname(io.listen_socket, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
    return UnavailableError("inspector: getsockname() failed");
  }
  port_ = ntohs(bound.sin_port);
  if (!SetNonBlocking(io.listen_socket)) {
    return UnavailableError("inspector: cannot make listen socket non-blocking");
  }

#if defined(_WIN32)
  io.wake_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (io.wake_socket == kInvalidSocket) {
    return UnavailableError("inspector: cannot create wake socket");
  }
  sockaddr_in wake{};
  wake.sin_family = AF_INET;
  wake.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  wake.sin_port = 0;
  int wake_len = sizeof(wake);
  if (bind(io.wake_socket, reinterpret_cast<const sockaddr*>(&wake), sizeof(wake)) != 0 ||
      getsockname(io.wake_socket, reinterpret_cast<sockaddr*>(&io.wake_addr), &wake_len) != 0 ||
      !SetNonBlocking(io.wake_socket)) {
    return UnavailableError("inspector: cannot set up wake socket");
  }
#else
  if (pipe(io.wake_pipe) != 0 || !SetNonBlocking(io.wake_pipe[0]) ||
      !SetNonBlocking(io.wake_pipe[1])) {
    return UnavailableError("inspector: cannot create wake pipe");
  }
#endif
  return Status::Ok();
}

std::string InspectorServer::WebSocketUrl() const {
  return "ws://127.0.0.1:" + std::to_string(port_) + "/" + options_.target_id;
}

std::string InspectorServer::DevToolsFrontendUrl() const {
  return "devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=127.0.0.1:" +
         std::to_string(port_) + "/" + options_.target_id;
}

void InspectorServer::Send(int session_id, std::string message) {
  if (outbound_.Push({session_id, std::move(message)})) {
    Wake();
  }
}

void InspectorServer::Wake() {
  const char byte = 1;
#if defined(_WIN32)
  sendto(io_->wake_socket, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&io_->wake_addr),
         sizeof(io_->wake_addr));
#else
  // 管道满(EAGAIN)说明已有未处理的唤醒, 忽略即可。
  [[maybe_unused]] const ssize_t n = write(io_->wake_pipe[1], &byte, 1);
#endif
}

// ---------------------------------------------------------------------------
// IO 线程

namespace {

class IoLoop {
 public:
  IoLoop(const InspectorServer::Options& options, int port,
         BlockingQueue<InspectorServer::Event>& inbound, const std::function<void()>& notify)
      : options_(options), port_(port), inbound_(inbound), notify_(notify) {}

  void Accept(SocketHandle listen_socket, std::map<int, std::unique_ptr<Connection>>& conns,
              int& next_id) {
    for (;;) {
      SocketHandle s = accept(listen_socket, nullptr, nullptr);
      if (s == kInvalidSocket) {
        const int err = LastSocketError();
        if (!IsWouldBlock(err) && !IsInterrupted(err)) {
          MOMO_LOG(WARNING) << "inspector: accept failed: " << SocketErrorString(err);
        }
        return;
      }
      if (!SetNonBlocking(s)) {
        CloseSocket(s);
        continue;
      }
      ConfigureAcceptedSocket(s);
      int one = 1;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
      auto conn = std::make_unique<Connection>();
      conn->id = next_id++;
      conn->socket = s;
      conns.emplace(conn->id, std::move(conn));
    }
  }

  void Read(Connection& conn) {
    char buffer[kReadChunk];
    for (;;) {
      const long n = RecvBytes(conn.socket, buffer, sizeof(buffer));
      if (n > 0) {
        conn.in.append(buffer, static_cast<size_t>(n));
        continue;
      }
      if (n == 0) {
        conn.dead = true;  // 对端关闭
        break;
      }
      const int err = LastSocketError();
      if (IsInterrupted(err)) {
        continue;
      }
      if (!IsWouldBlock(err)) {
        conn.dead = true;
      }
      break;
    }
    if (!conn.close_after_write) {
      Process(conn);
    }
  }

  void Write(Connection& conn) {
    while (!conn.out.empty()) {
      const long n = SendBytes(conn.socket, conn.out.data(), conn.out.size());
      if (n > 0) {
        conn.out.erase(0, static_cast<size_t>(n));
        continue;
      }
      const int err = LastSocketError();
      if (n < 0 && IsInterrupted(err)) {
        continue;
      }
      if (n < 0 && IsWouldBlock(err)) {
        return;
      }
      conn.dead = true;
      return;
    }
    if (conn.close_after_write) {
      conn.dead = true;
    }
  }

  void QueueText(Connection& conn, std::string_view text) {
    if (conn.state != Connection::State::kWebSocket || conn.close_sent || conn.dead) {
      return;
    }
    conn.out += wire::EncodeWsFrame(wire::WsOpcode::kText, text);
  }

  void Push(InspectorServer::Event event) {
    if (inbound_.Push(std::move(event)) && notify_) {
      notify_();
    }
  }

 private:
  void Process(Connection& conn) {
    if (conn.state == Connection::State::kHttp) {
      wire::HttpRequest request;
      size_t consumed = 0;
      switch (wire::ParseHttpRequest(conn.in, &request, &consumed)) {
        case wire::ParseResult::kIncomplete:
          if (conn.in.size() > kMaxHttpHeaderSize) {
            Respond(conn, TextResponse(431, "Request Header Fields Too Large"));
          }
          return;
        case wire::ParseResult::kError:
          Respond(conn, TextResponse(400, "Bad Request"));
          return;
        case wire::ParseResult::kOk:
          conn.in.erase(0, consumed);
          HandleHttp(conn, request);
          break;
      }
    }
    if (conn.state == Connection::State::kWebSocket) {
      ProcessFrames(conn);
    }
  }

  void Respond(Connection& conn, std::string response) {
    conn.out += response;
    conn.in.clear();
    conn.close_after_write = true;
  }

  void HandleHttp(Connection& conn, const wire::HttpRequest& request) {
    const std::string* host = request.Header("host");
    if (host == nullptr || !wire::IsLoopbackHost(*host)) {
      Respond(conn, TextResponse(403, "Forbidden: Host must be localhost or 127.0.0.1"));
      return;
    }
    if (request.method != "GET") {
      Respond(conn, TextResponse(405, "Method Not Allowed"));
      return;
    }
    const std::string* upgrade = request.Header("upgrade");
    if (upgrade != nullptr && wire::HeaderHasToken(*upgrade, "websocket")) {
      HandleUpgrade(conn, request);
      return;
    }
    if (request.path == "/json" || request.path == "/json/list") {
      Respond(conn, HttpResponse(200, "OK", "application/json; charset=UTF-8", TargetList()));
    } else if (request.path == "/json/version") {
      Respond(conn, HttpResponse(200, "OK", "application/json; charset=UTF-8", Version()));
    } else {
      Respond(conn, TextResponse(404, "Not Found"));
    }
  }

  void HandleUpgrade(Connection& conn, const wire::HttpRequest& request) {
    if (request.path != "/" + options_.target_id) {
      Respond(conn, TextResponse(404, "Not Found"));
      return;
    }
    const std::string* connection = request.Header("connection");
    const std::string* key = request.Header("sec-websocket-key");
    const std::string* version = request.Header("sec-websocket-version");
    if (connection == nullptr || !wire::HeaderHasToken(*connection, "upgrade") ||
        key == nullptr || key->empty() || version == nullptr || *version != "13") {
      Respond(conn, TextResponse(400, "Bad WebSocket Upgrade Request"));
      return;
    }
    conn.out +=
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        wire::WebSocketAcceptKey(*key) + "\r\n\r\n";
    conn.state = Connection::State::kWebSocket;
    conn.announced = true;
    Push({InspectorServer::Event::Type::kConnected, conn.id, {}});
  }

  void ProcessFrames(Connection& conn) {
    while (!conn.close_after_write) {
      wire::WsFrame frame;
      size_t consumed = 0;
      const wire::ParseResult result =
          wire::ParseWsFrame(conn.in, kMaxMessageSize, &frame, &consumed);
      if (result == wire::ParseResult::kIncomplete) {
        return;
      }
      if (result == wire::ParseResult::kError) {
        Fail(conn, kCloseProtocolError);
        return;
      }
      conn.in.erase(0, consumed);
      if (!frame.masked) {  // RFC 6455 §5.1: 客户端帧必须掩码
        Fail(conn, kCloseProtocolError);
        return;
      }
      switch (frame.opcode) {
        case wire::WsOpcode::kText:
        case wire::WsOpcode::kBinary:
          if (conn.in_message) {
            Fail(conn, kCloseProtocolError);
            return;
          }
          if (frame.fin) {
            Push({InspectorServer::Event::Type::kMessage, conn.id, std::move(frame.payload)});
          } else {
            conn.in_message = true;
            conn.message = std::move(frame.payload);
          }
          break;
        case wire::WsOpcode::kContinuation:
          if (!conn.in_message) {
            Fail(conn, kCloseProtocolError);
            return;
          }
          if (conn.message.size() + frame.payload.size() > kMaxMessageSize) {
            Fail(conn, kCloseTooBig);
            return;
          }
          conn.message += frame.payload;
          if (frame.fin) {
            conn.in_message = false;
            Push({InspectorServer::Event::Type::kMessage, conn.id, std::move(conn.message)});
            conn.message.clear();
          }
          break;
        case wire::WsOpcode::kPing:
          conn.out += wire::EncodeWsFrame(wire::WsOpcode::kPong, frame.payload);
          break;
        case wire::WsOpcode::kPong:
          break;
        case wire::WsOpcode::kClose: {
          // 回显对端的状态码后关闭。
          const std::string payload = frame.payload.size() >= 2
                                          ? frame.payload.substr(0, 2)
                                          : wire::EncodeWsClosePayload(kCloseNormal);
          SendClose(conn, payload);
          return;
        }
      }
    }
  }

  void Fail(Connection& conn, uint16_t code) {
    SendClose(conn, wire::EncodeWsClosePayload(code));
    conn.in.clear();
  }

 public:
  void SendClose(Connection& conn, std::string_view payload) {
    if (!conn.close_sent) {
      conn.out += wire::EncodeWsFrame(wire::WsOpcode::kClose, payload);
      conn.close_sent = true;
    }
    conn.close_after_write = true;
  }

 private:
  std::string Host() const { return "127.0.0.1:" + std::to_string(port_); }

  std::string TargetList() const {
    const std::string ws = Host() + "/" + options_.target_id;
    std::string out = "[ {\n";
    out += "  \"description\": \"" + wire::JsonEscape(options_.description) + "\",\n";
    out += "  \"devtoolsFrontendUrl\": \"devtools://devtools/bundled/js_app.html?experiments=true"
           "&v8only=true&ws=" + ws + "\",\n";
    out += "  \"devtoolsFrontendUrlCompat\": \"devtools://devtools/bundled/inspector.html?"
           "experiments=true&v8only=true&ws=" + ws + "\",\n";
    out += "  \"id\": \"" + wire::JsonEscape(options_.target_id) + "\",\n";
    out += "  \"title\": \"" + wire::JsonEscape(options_.title) + "\",\n";
    out += "  \"type\": \"node\",\n";  // chrome://inspect 按 Node 目标展示
    out += "  \"url\": \"" + wire::JsonEscape(options_.url) + "\",\n";
    out += "  \"webSocketDebuggerUrl\": \"ws://" + ws + "\"\n";
    out += "} ]\n";
    return out;
  }

  std::string Version() const {
    return "{\n  \"Browser\": \"" + wire::JsonEscape(options_.browser) +
           "\",\n  \"Protocol-Version\": \"1.1\"\n}\n";
  }

  const InspectorServer::Options& options_;
  int port_;
  BlockingQueue<InspectorServer::Event>& inbound_;
  const std::function<void()>& notify_;
};

}  // namespace

void InspectorServer::IoMain() {
  IoState& io = *io_;
  IoLoop loop(options_, port_, *inbound_, notify_);
  std::vector<PollFd> fds;
  std::vector<Connection*> polled;

  while (!stop_.load(std::memory_order_acquire)) {
    // 1) 出站消息分发到连接并尽量立即写出。
    while (std::optional<std::pair<int, std::string>> msg = outbound_.TryPop()) {
      auto it = io.connections.find(msg->first);
      if (it != io.connections.end()) {
        loop.QueueText(*it->second, msg->second);
      }
    }
    for (auto& [id, conn] : io.connections) {
      if (!conn->dead && (!conn->out.empty() || conn->close_after_write)) {
        loop.Write(*conn);
      }
    }

    // 2) 回收死连接。
    for (auto it = io.connections.begin(); it != io.connections.end();) {
      if (!it->second->dead) {
        ++it;
        continue;
      }
      CloseSocket(it->second->socket);
      if (it->second->announced) {
        loop.Push({Event::Type::kDisconnected, it->first, {}});
      }
      it = io.connections.erase(it);
    }

    // 3) 等待事件。
    fds.clear();
    polled.clear();
#if defined(_WIN32)
    fds.push_back({io.wake_socket, POLLIN, 0});
#else
    fds.push_back({io.wake_pipe[0], POLLIN, 0});
#endif
    fds.push_back({io.listen_socket, POLLIN, 0});
    for (auto& [id, conn] : io.connections) {
      short events = POLLIN;
      if (!conn->out.empty()) {
        events |= POLLOUT;
      }
      fds.push_back({conn->socket, events, 0});
      polled.push_back(conn.get());
    }
    const int rc = PollSockets(fds.data(), fds.size(), -1);
    if (rc < 0) {
      const int err = LastSocketError();
      if (IsInterrupted(err)) {
        continue;
      }
      MOMO_LOG(ERROR) << "inspector: poll failed: " << SocketErrorString(err);
      break;
    }

    // 4) 处理就绪的描述符。
    if (fds[0].revents != 0) {
      char drain[256];
#if defined(_WIN32)
      while (recv(io.wake_socket, drain, sizeof(drain), 0) > 0) {
      }
#else
      while (read(io.wake_pipe[0], drain, sizeof(drain)) > 0) {
      }
#endif
    }
    if (fds[1].revents != 0) {
      loop.Accept(io.listen_socket, io.connections, io.next_id);
    }
    for (size_t i = 0; i < polled.size(); ++i) {
      const short revents = fds[i + 2].revents;
      Connection& conn = *polled[i];
      if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        loop.Read(conn);
      }
      if ((revents & POLLOUT) != 0 && !conn.dead) {
        loop.Write(conn);
      }
    }
  }

  // 关闭: 尽力通知对端(非阻塞, 不保证送达)。不再推送会话事件 —— 消费方(Inspector)正在析构,
  // 自行拆除全部会话。
  for (auto& [id, conn] : io.connections) {
    if (conn->state == Connection::State::kWebSocket && !conn->dead) {
      loop.SendClose(*conn, wire::EncodeWsClosePayload(kCloseGoingAway));
      loop.Write(*conn);
    }
    CloseSocket(conn->socket);
    conn->socket = kInvalidSocket;
  }
  io.connections.clear();
}

}  // namespace momo::engine::runtime
