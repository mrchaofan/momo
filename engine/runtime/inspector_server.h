#pragma once

// InspectorServer: Inspector 的网络端 —— 只监听 127.0.0.1 的最小 HTTP + WebSocket(RFC 6455)服务器。
// 无 V8 依赖;协议细节见 inspector_wire.h。只能被 engine/runtime 的实现与测试包含。
//
// 端点(与 Node --inspect 兼容, Chrome 的 chrome://inspect 与 DevTools 可直接发现):
//   GET /json, /json/list  目标列表(webSocketDebuggerUrl、devtoolsFrontendUrl…)
//   GET /json/version      {"Browser", "Protocol-Version"}
//   GET /<target_id> + Upgrade: websocket   建立调试会话(每个连接一个会话 id)
// 所有请求校验 Host 头为回环地址(防 DNS rebinding)。
//
// 线程约定:
//   - Start/析构在同一线程(Inspector 的 ui 线程)。析构 = 停止 IO 线程并 join, 关闭全部连接
//     (析构期间不再推送会话事件)。
//   - 网络 IO 全部在自有的 momo::Thread("momo.inspector.io")上进行(poll 循环)。
//   - 入站: IO 线程把会话事件推入调用方提供的 inbound 队列(BlockingQueue, 由 Inspector 拥有, 进程内
//     会话也往里推), 每推一个就把 notify 投递到调用方指定的 notify_runner(ui 线程)上执行一次——
//     notify 绝不在 IO 线程上直接调用。消费者(ui 线程)用 TryPop 排空,
//     或在被同步阻塞时(断点暂停)用 Pop 阻塞等待。
//   - 出站: Send() 任意线程, 消息入队后唤醒 IO 线程发送;会话已断开则丢弃。

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "base/blocking_queue.h"
#include "base/macros.h"
#include "base/status.h"
#include "base/task_runner.h"

namespace momo {
class Thread;
}

namespace momo::engine::runtime {

class InspectorServer {
 public:
  struct Options {
    int port = 9229;          // 0 = 由系统分配(port() 取实际端口)
    std::string target_id;    // WebSocket 路径 "/<target_id>"
    std::string title;        // /json 中的 title
    std::string url;          // /json 中的 url
    std::string browser;      // /json/version 中的 Browser
    std::string description = "momo instance";
  };

  struct Event {
    enum class Type { kConnected, kMessage, kDisconnected };
    Type type = Type::kMessage;
    int session_id = 0;
    std::string message;  // kMessage: 完整的文本消息(UTF-8)
  };

  using EventQueue = BlockingQueue<Event>;

  // 绑定 127.0.0.1:port 并启动 IO 线程。端口被占用等错误同步返回。
  // inbound: 会话事件的去处(共享, 服务器析构后调用方仍可使用)。
  // notify: 每个事件入队后投递到 notify_runner 上执行一次(回调在注册方线程执行)。
  static StatusOr<std::unique_ptr<InspectorServer>> Start(Options options,
                                                          std::shared_ptr<EventQueue> inbound,
                                                          std::shared_ptr<TaskRunner> notify_runner,
                                                          std::function<void()> notify);
  ~InspectorServer();

  int port() const { return port_; }
  const std::string& target_id() const { return options_.target_id; }
  // ws://127.0.0.1:<port>/<target_id>
  std::string WebSocketUrl() const;
  // devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=127.0.0.1:<port>/<id>
  std::string DevToolsFrontendUrl() const;

  // 向会话发送一条文本消息(任意线程)。
  void Send(int session_id, std::string message);

 private:
  struct IoState;

  InspectorServer(Options options, std::shared_ptr<EventQueue> inbound,
                  std::function<void()> post_notify);
  Status Listen();
  void Wake();
  void IoMain();  // IO 线程: poll 循环, 直到 stop_

  Options options_;
  std::function<void()> notify_;  // 投递 notify 到 notify_runner(IO 线程调用的是这个投递, 不是 notify)
  int port_ = 0;

  std::shared_ptr<EventQueue> inbound_;
  BlockingQueue<std::pair<int, std::string>> outbound_;
  std::atomic<bool> stop_{false};

  std::unique_ptr<IoState> io_;      // 套接字与连接表;Start 后仅 IO 线程访问
  std::unique_ptr<Thread> thread_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(InspectorServer);
};

}  // namespace momo::engine::runtime
