#pragma once

// Inspector: V8 Inspector + Chrome DevTools 调试服务(docs/04 §7)。公共头不含 V8 类型。
//
// 用法(ui 线程, JsRuntime 创建之后、执行主模块之前):
//   MOMO_ASSIGN_OR_RETURN(auto inspector, Inspector::Start(*runtime, {.port = 9229}));
//   runtime->RunMainModule(bundle);
//   ...
//   inspector.reset();  // 必须先于 JsRuntime 析构
//
// 能力: 断点/单步、console.* 送到 DevTools(同时仍写引擎日志)、Runtime.evaluate、CPU/堆 profile
// (V8 内建域)。发现端点 GET /json、/json/list、/json/version;只监听 127.0.0.1。
//
// 线程设计:
//   - 网络 IO 在 InspectorServer 自有的 "momo.inspector.io" 线程;协议消息全部在 ui 线程分发给 V8。
//   - 入站消息经线程安全的 BlockingQueue 交给 ui 线程。正常运行时 IO 线程每收到一条就向 ui 的
//     EventLoop 投递一个"排空队列"任务(WeakPtr 判活)。
//   - 断点暂停时 V8 在 ui 线程上同步调用 runMessageLoopOnPause, EventLoop 不可嵌套运行, 因此在此
//     直接阻塞在该队列的 Pop 上逐条分发, 直到 V8 回调 quitMessageLoopOnPause(Debugger.resume 等)
//     或所有会话断开。暂停期间 ui 线程的其他任务(帧、定时器)都不执行, 这是预期行为。
//   - wait_for_debugger 时 Start 以同样方式阻塞, 直到 DevTools 发来 Runtime.runIfWaitingForDebugger。
//   - 进程内会话(ConnectLocal, 对标 Node 的 inspector.Session)与网络会话共用同一入站队列与分发路径,
//     用于测试与将来的进程内工具;暂停期间同样能送达。
//
// 线程约定: momo::Object, 归属 ui 线程(须与 JsRuntime 同线程), 仅在该线程创建、调用与析构。
// 生命周期: 不得长于传入的 JsRuntime。析构 = 断开全部会话、关闭服务器、join 网络线程。

#include <functional>
#include <memory>
#include <string>

#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"

namespace momo::engine::runtime {

class JsRuntime;

class Inspector final : public Object {
 public:
  struct Options {
    int port = 9229;                 // 0 = 由系统分配, 用 port() 取实际端口;< 0 = 不启动网络服务
                                     // (仅进程内会话)
    bool wait_for_debugger = false;  // Start 阻塞到调试器发来 Runtime.runIfWaitingForDebugger
    std::string target_title;        // DevTools 目标列表中的标题;空则为 "Momo"
    std::string target_url;          // 目标 url(如主模块的 file:// 路径), 可空
  };

  // 进程内调试会话的句柄。Send/析构可在任意线程调用;Inspector 析构后 Send 无效果。
  // 析构 = 断开会话(若因本会话处于断点暂停, 执行随之继续)。
  class LocalSession {
   public:
    ~LocalSession();
    // 发送一条 CDP 消息(JSON 文本), 在 ui 线程分发(断点暂停期间也会被处理)。
    void Send(std::string message);

   private:
    friend class Inspector;
    struct Shared;
    explicit LocalSession(std::shared_ptr<Shared> shared, int id);

    std::shared_ptr<Shared> shared_;
    int id_;

    MOMO_DISALLOW_COPY_AND_ASSIGN(LocalSession);
  };

  static StatusOr<std::unique_ptr<Inspector>> Start(JsRuntime& runtime, const Options& options);
  ~Inspector() override;

  // 实际监听端口;未启动网络服务时为 -1。
  int port() const;
  // ws://127.0.0.1:<port>/<id>;未启动网络服务时为空。
  std::string websocket_url() const;
  // devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=127.0.0.1:<port>/<id>
  std::string devtools_frontend_url() const;

  // 建立进程内会话(ui 线程)。on_message 在 ui 线程收到 V8 发出的每条响应/通知(UTF-8 JSON),
  // 不得在其中销毁 Inspector。
  std::unique_ptr<LocalSession> ConnectLocal(std::function<void(const std::string&)> on_message);
  // 当前已连接的调试会话数。
  int session_count() const;

  // 阻塞直到某个调试会话发来 Runtime.runIfWaitingForDebugger(Options::wait_for_debugger 的实现,
  // 也可单独调用)。期间照常处理 DevTools 消息。
  void WaitForDebugger();

 private:
  class Impl;

  explicit Inspector(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Inspector);
};

}  // namespace momo::engine::runtime
