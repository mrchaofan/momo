#include "engine/runtime/inspector.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "engine/runtime/inspector_server.h"
#include "engine/runtime/inspector_wire.h"
#include "engine/runtime/js_runtime.h"
#include "engine/runtime/js_runtime_impl.h"
#include "v8-inspector.h"
#include "v8.h"

namespace momo::engine::runtime {

namespace {

// JsRuntime 只有一个主 context, 放在固定的 context group 中。
constexpr int kContextGroupId = 1;

// UTF-8 → v8_inspector::StringView。StringView 的 8 位形式是 Latin-1 而非 UTF-8,
// 所以非 ASCII 内容转成 UTF-16。view() 的生命周期不超过本对象。
class Utf8View {
 public:
  explicit Utf8View(std::string_view utf8) {
    if (wire::IsAscii(utf8)) {
      view_ = v8_inspector::StringView(reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size());
    } else {
      utf16_ = wire::Utf8ToUtf16(utf8);
      view_ = v8_inspector::StringView(reinterpret_cast<const uint16_t*>(utf16_.data()),
                                       utf16_.size());
    }
  }
  const v8_inspector::StringView& view() const { return view_; }

 private:
  std::u16string utf16_;
  v8_inspector::StringView view_;
};

std::string ToUtf8(const v8_inspector::StringView& view) {
  if (view.is8Bit()) {
    return wire::Latin1ToUtf8(view.characters8(), view.length());
  }
  return wire::Utf16ToUtf8(reinterpret_cast<const char16_t*>(view.characters16()), view.length());
}

// 随机 UUID v4 形式的目标 id(出现在 WebSocket 路径中, 不可预测以免被其他本地网页猜到)。
std::string GenerateTargetId() {
  std::random_device device;
  std::mt19937_64 rng((uint64_t{device()} << 32) ^ device());
  uint8_t bytes[16];
  for (int i = 0; i < 16; i += 8) {
    const uint64_t v = rng();
    for (int k = 0; k < 8; ++k) {
      bytes[i + k] = static_cast<uint8_t>(v >> (k * 8));
    }
  }
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
  char out[37];
  std::snprintf(out, sizeof(out),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
                bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
                bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 进程内会话句柄与 Impl 共享的部分: 入站队列 + 唤醒 ui 的回调。任意线程使用。

struct Inspector::LocalSession::Shared {
  std::shared_ptr<InspectorServer::EventQueue> inbound;
  std::shared_ptr<TaskRunner> ui_runner;
  std::function<void()> notify;  // "排空队列"(在 ui 线程执行, WeakPtr 判活)

  // 任意线程: 入队后把 notify 投递到 ui 线程执行(不在推送方线程直接调用)。
  void Push(InspectorServer::Event event) const {
    if (inbound->Push(std::move(event))) {
      ui_runner->PostTask(notify);
    }
  }
};

Inspector::LocalSession::LocalSession(std::shared_ptr<Shared> shared, int id)
    : shared_(std::move(shared)), id_(id) {}

Inspector::LocalSession::~LocalSession() {
  shared_->Push({InspectorServer::Event::Type::kDisconnected, id_, {}});
}

void Inspector::LocalSession::Send(std::string message) {
  shared_->Push({InspectorServer::Event::Type::kMessage, id_, std::move(message)});
}

// ---------------------------------------------------------------------------
// Inspector::Impl: V8InspectorClient + 会话管理。除注明外全部在 ui 线程。

class Inspector::Impl final : public v8_inspector::V8InspectorClient {
 public:
  using Sink = std::function<void(std::string)>;  // 把 V8 发出的消息交给会话的对端(ui 线程调用)

  Impl(JsRuntimeImpl& runtime, const Options& options) : runtime_(runtime), options_(options) {}

  ~Impl() override {
    v8::HandleScope handle_scope(runtime_.isolate());
    // 先拆会话(其间 V8 可能还会经 Channel 发消息, 服务器仍在), 再关服务器, 最后注销 context。
    sessions_.clear();
    retired_.clear();
    server_.reset();
    if (shared_ != nullptr) {
      shared_->inbound->Close();  // 之后 LocalSession::Send/析构的推送被丢弃
    }
    if (v8_inspector_ != nullptr) {
      v8_inspector_->contextDestroyed(runtime_.context());
      v8_inspector_.reset();
    }
  }

  // notify: 在 ui 线程(ui_runner)上执行一次 DrainInbound;由事件来源投递过来。
  Status Start(std::shared_ptr<TaskRunner> ui_runner, std::function<void()> notify) {
    shared_ = std::make_shared<LocalSession::Shared>();
    shared_->inbound = std::make_shared<InspectorServer::EventQueue>();
    shared_->ui_runner = ui_runner;
    shared_->notify = notify;

    // 先起服务器: 端口占用等失败时不触碰 V8。之后到达的事件经 ui 任务处理, 晚于本函数返回。
    const std::string title = options_.target_title.empty() ? "Momo" : options_.target_title;
    if (options_.port >= 0) {
      InspectorServer::Options server_options;
      server_options.port = options_.port;
      server_options.target_id = GenerateTargetId();
      server_options.title = title;
      server_options.url = options_.target_url;
      server_options.browser = std::string("Momo/V8-") + v8::V8::GetVersion();
      MOMO_ASSIGN_OR_RETURN(server_, InspectorServer::Start(std::move(server_options),
                                                            shared_->inbound, std::move(ui_runner),
                                                            std::move(notify)));
    }

    v8::Isolate* isolate = runtime_.isolate();
    v8::HandleScope handle_scope(isolate);
    v8_inspector_ = v8_inspector::V8Inspector::create(isolate, this);
    Utf8View name(title);
    v8_inspector::V8ContextInfo info(runtime_.context(), kContextGroupId, name.view());
    static constexpr char kAuxData[] = R"({"isDefault":true})";  // DevTools 据此选默认 context
    info.auxData = v8_inspector::StringView(reinterpret_cast<const uint8_t*>(kAuxData),
                                            sizeof(kAuxData) - 1);
    v8_inspector_->contextCreated(info);

    if (server_ != nullptr) {
      MOMO_LOG(INFO) << "Debugger listening on " << server_->WebSocketUrl();
      MOMO_LOG(INFO) << "For help, open chrome://inspect or " << server_->DevToolsFrontendUrl();
    }
    return Status::Ok();
  }

  InspectorServer* server() const { return server_.get(); }
  int session_count() const { return static_cast<int>(sessions_.size()); }

  std::unique_ptr<LocalSession> ConnectLocal(std::function<void(const std::string&)> on_message) {
    const int id = next_local_id_--;  // 负数, 与服务器分配的正数会话 id 不冲突
    AddSession(id, [on_message = std::move(on_message)](std::string message) {
      on_message(message);
    });
    return std::unique_ptr<LocalSession>(new LocalSession(shared_, id));
  }

  // ui 任务: 排空入站队列(每入队一个事件投递一次, 多余的任务看到空队列直接返回)。
  void DrainInbound() {
    while (std::optional<InspectorServer::Event> event = shared_->inbound->TryPop()) {
      HandleEvent(std::move(*event));
    }
    if (dispatch_depth_ == 0) {
      runtime_.DrainMicrotasks();  // Runtime.evaluate 等可能入队了 microtask
    }
  }

  void WaitForDebugger() {
    waiting_for_debugger_ = true;
    MOMO_LOG(INFO) << "Waiting for the debugger to send Runtime.runIfWaitingForDebugger ...";
    PumpWhile([this] { return waiting_for_debugger_; });
    waiting_for_debugger_ = false;
  }

  // ---- V8InspectorClient ----

  void runMessageLoopOnPause(int /*context_group_id*/) override {
    if (in_pause_loop_) {
      return;  // V8 不应嵌套暂停;防御性返回
    }
    in_pause_loop_ = true;
    paused_ = true;
    // 无存活会话(都已断开)时不阻塞: 没人能发 resume。
    PumpWhile([this] { return paused_ && !sessions_.empty(); });
    paused_ = false;
    in_pause_loop_ = false;
  }

  void quitMessageLoopOnPause() override { paused_ = false; }

  void runIfWaitingForDebugger(int /*context_group_id*/) override {
    waiting_for_debugger_ = false;
  }

  double currentTimeMS() override {
    return std::chrono::duration<double, std::milli>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  v8::Local<v8::Context> ensureDefaultContextInGroup(int context_group_id) override {
    if (context_group_id != kContextGroupId) {
      return {};
    }
    return runtime_.context();
  }

 private:
  // 一个调试会话(DevTools 连接或进程内会话)对应一个 V8InspectorSession。
  class Session final : public v8_inspector::V8Inspector::Channel {
   public:
    explicit Session(Sink sink) : sink_(std::move(sink)) {}
    ~Session() override { session_.reset(); }

    void Connect(v8_inspector::V8Inspector& inspector, bool waiting_for_debugger) {
      session_ = inspector.connect(
          kContextGroupId, this, v8_inspector::StringView(),
          v8_inspector::V8Inspector::kFullyTrusted,
          waiting_for_debugger ? v8_inspector::V8Inspector::kWaitingForDebugger
                               : v8_inspector::V8Inspector::kNotWaitingForDebugger);
    }

    void Dispatch(const std::string& message) {
      Utf8View view(message);
      session_->dispatchProtocolMessage(view.view());
    }

    v8_inspector::V8InspectorSession& v8_session() { return *session_; }
    // 断开后不再向对端发送(V8 在销毁会话前仍可能回调 Channel)。
    void Detach() { sink_ = nullptr; }

    // Channel
    void sendResponse(int /*call_id*/,
                      std::unique_ptr<v8_inspector::StringBuffer> message) override {
      Forward(message->string());
    }
    void sendNotification(std::unique_ptr<v8_inspector::StringBuffer> message) override {
      Forward(message->string());
    }
    void flushProtocolNotifications() override {}

   private:
    void Forward(const v8_inspector::StringView& message) {
      if (sink_) {
        sink_(ToUtf8(message));
      }
    }

    Sink sink_;
    std::unique_ptr<v8_inspector::V8InspectorSession> session_;
  };

  void AddSession(int id, Sink sink) {
    auto session = std::make_unique<Session>(std::move(sink));
    Session* raw = session.get();
    sessions_[id] = std::move(session);
    v8::HandleScope handle_scope(runtime_.isolate());
    raw->Connect(*v8_inspector_, waiting_for_debugger_);
  }

  // 在 ui 线程阻塞地逐条处理入站事件, 直到 condition() 为假或队列关闭。
  // 用于断点暂停与等待调试器 —— 此时不能(也不必)回到 EventLoop。
  template <typename Condition>
  void PumpWhile(Condition condition) {
    while (condition()) {
      std::optional<InspectorServer::Event> event = shared_->inbound->Pop();
      if (!event.has_value()) {
        return;  // 队列已关闭
      }
      HandleEvent(std::move(*event));
    }
  }

  void HandleEvent(InspectorServer::Event event) {
    using Type = InspectorServer::Event::Type;
    switch (event.type) {
      case Type::kConnected: {
        InspectorServer* server = server_.get();
        if (server == nullptr) {
          return;
        }
        const int id = event.session_id;
        AddSession(id, [server, id](std::string message) { server->Send(id, std::move(message)); });
        MOMO_LOG(INFO) << "Debugger attached (session " << id << ").";
        break;
      }
      case Type::kMessage: {
        auto it = sessions_.find(event.session_id);
        if (it == sessions_.end()) {
          return;
        }
        Session* session = it->second.get();
        ++dispatch_depth_;
        {
          v8::HandleScope handle_scope(runtime_.isolate());
          session->Dispatch(event.message);
        }
        --dispatch_depth_;
        if (dispatch_depth_ == 0) {
          retired_.clear();
        }
        break;
      }
      case Type::kDisconnected: {
        auto it = sessions_.find(event.session_id);
        if (it == sessions_.end()) {
          return;
        }
        std::unique_ptr<Session> session = std::move(it->second);
        sessions_.erase(it);
        session->Detach();
        if (event.session_id > 0) {
          MOMO_LOG(INFO) << "Debugger detached (session " << event.session_id << ").";
        }
        if (dispatch_depth_ > 0) {
          // 可能正处在该会话的 dispatchProtocolMessage 栈内(如 Runtime.evaluate 触发了暂停),
          // 不能就地销毁: 屏蔽其后续暂停, 待最外层分发返回后再销毁。
          session->v8_session().setSkipAllPauses(true);
          retired_.push_back(std::move(session));
        } else {
          v8::HandleScope handle_scope(runtime_.isolate());
          session.reset();  // V8 会随之结束该会话造成的暂停
        }
        break;
      }
    }
  }

  JsRuntimeImpl& runtime_;
  const Options options_;
  std::shared_ptr<LocalSession::Shared> shared_;
  std::unique_ptr<v8_inspector::V8Inspector> v8_inspector_;
  std::unique_ptr<InspectorServer> server_;
  std::map<int, std::unique_ptr<Session>> sessions_;
  std::vector<std::unique_ptr<Session>> retired_;  // 已断开、待最外层分发返回后销毁
  int next_local_id_ = -1;
  int dispatch_depth_ = 0;
  bool in_pause_loop_ = false;
  bool paused_ = false;
  bool waiting_for_debugger_ = false;
};

// ---------------------------------------------------------------------------

StatusOr<std::unique_ptr<Inspector>> Inspector::Start(JsRuntime& runtime, const Options& options) {
  if (!runtime.IsOnAffinityThread()) {
    return FailedPreconditionError("Inspector must be started on the JsRuntime's thread");
  }
  std::unique_ptr<Inspector> inspector(
      new Inspector(std::make_unique<Impl>(runtime.impl(), options)));
  // 每入队一个事件就在 ui 线程(调用方线程)执行一次排空;WeakPtr 拦住 Inspector 析构后才到达的任务。
  WeakPtr<Inspector> weak(inspector.get());
  MOMO_RETURN_IF_ERROR(inspector->impl_->Start(runtime.task_runner(), [weak] {
    if (Inspector* self = weak.get()) {
      self->impl_->DrainInbound();
    }
  }));
  if (options.wait_for_debugger) {
    inspector->WaitForDebugger();
  }
  return inspector;
}

Inspector::Inspector(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Inspector::~Inspector() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  InvalidateWeakPtrs();
  impl_.reset();
}

int Inspector::port() const {
  return impl_->server() != nullptr ? impl_->server()->port() : -1;
}

std::string Inspector::websocket_url() const {
  return impl_->server() != nullptr ? impl_->server()->WebSocketUrl() : std::string();
}

std::string Inspector::devtools_frontend_url() const {
  return impl_->server() != nullptr ? impl_->server()->DevToolsFrontendUrl() : std::string();
}

std::unique_ptr<Inspector::LocalSession> Inspector::ConnectLocal(
    std::function<void(const std::string&)> on_message) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  return impl_->ConnectLocal(std::move(on_message));
}

int Inspector::session_count() const {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  return impl_->session_count();
}

void Inspector::WaitForDebugger() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  impl_->WaitForDebugger();
}

}  // namespace momo::engine::runtime
