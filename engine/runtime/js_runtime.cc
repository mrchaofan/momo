#include "engine/runtime/js_runtime.h"

#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "base/event_loop.h"
#include "base/logging.h"
#include "engine/runtime/js_args.h"
#include "engine/runtime/node_bindings.h"
#include "engine/runtime/node_bootstrap.h"
#include "engine/runtime/wrappable.h"
#include "node.h"
#include "uv.h"

namespace momo::engine::runtime {

namespace {

constexpr uint32_t kIsolateSlotRuntime = 0;

// ---------------------------------------------------------------------------
// Node 进程级初始化: 每进程恰好一次(InitializeOncePerProcess 内含 V8 平台、V8、ICU、OpenSSL 的初始化),
// 之后不能重新初始化。这是 docs/10「禁全局可变状态」的必要例外: 只在这里、只写一次、永不释放。
//   - 不改动 stdio 与 TTY 状态、不安装 Node 的信号处理(宿主应用自己管理);
//   - 不读 NODE_OPTIONS(用户机器上的环境变量不应改变应用行为);
//   - inspector(--inspect)是进程级选项: 以第一次创建运行时时的 Options 为准。

struct NodeProcess {
  std::shared_ptr<node::InitializationResult> result;
};

NodeProcess& InitializeNodeOnce(const std::vector<std::string>& extra_args) {
  static std::once_flag once;
  static NodeProcess* process = nullptr;  // 有意泄漏: 进程退出前不可拆除
  std::call_once(once, [&extra_args] {
    process = new NodeProcess();
    std::vector<std::string> args = {"momo"};
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    process->result = node::InitializeOncePerProcess(
        args, {node::ProcessInitializationFlags::kNoStdioInitialization,
               node::ProcessInitializationFlags::kNoDefaultSignalHandling,
               node::ProcessInitializationFlags::kDisableNodeOptionsEnv,
               node::ProcessInitializationFlags::kNoPrintHelpOrVersionOutput});
    for (const std::string& error : process->result->errors()) {
      MOMO_LOG(ERROR) << "Node initialization: " << error;
    }
    MOMO_CHECK(process->result->exit_code() == 0 && process->result->platform() != nullptr)
        << "Node initialization failed";
    MOMO_LOG(INFO) << "Node " << NODE_VERSION << " (V8 " << v8::V8::GetVersion() << ", libuv "
                   << uv_version_string() << ") initialized.";
  });
  return *process;
}

}  // namespace

// ---------------------------------------------------------------------------
// 创建与销毁

JsRuntime::JsRuntime() = default;

StatusOr<std::unique_ptr<JsRuntime>> JsRuntime::Create(const Options& options) {
  EventLoop* loop = EventLoop::Current();
  if (loop == nullptr) {
    return FailedPreconditionError("JsRuntime must be created on a thread running an EventLoop");
  }
  std::shared_ptr<TaskRunner> task_runner = loop->GetTaskRunner();
  // 进程级的 Node 选项: 只在进程内第一次创建时生效。
  std::vector<std::string> node_args;
  if (options.enable_inspector) {
    node_args.push_back(std::string(options.inspector_wait ? "--inspect-wait" : "--inspect") +
                        "=127.0.0.1:" + std::to_string(options.inspector_port));
  }
  NodeProcess& process = InitializeNodeOnce(node_args);
  std::unique_ptr<JsRuntime> runtime(new JsRuntime());

  std::vector<std::string> errors;
  runtime->setup_ = node::CommonEnvironmentSetup::Create(
      process.result->platform(), &errors, process.result->args(), process.result->exec_args(),
      node::EnvironmentFlags::kDefaultFlags);
  if (runtime->setup_ == nullptr) {
    std::string message = "failed to create the Node.js environment";
    for (const std::string& error : errors) {
      message += "; " + error;
    }
    return InternalError(message);
  }
  v8::Isolate* isolate = runtime->setup_->isolate();
  runtime->isolate_ = isolate;
  runtime->env_ = runtime->setup_->env();
  // process.exit() 不得直接 exit(): 其他线程(platform、raster)还在运行, 有序关闭由宿主完成
  // (同 Electron 把 process.exit 接到 app.exit)。停止 JS 执行后交给宿主。
  node::SetProcessExitHandler(
      runtime->env_, [on_exit = options.on_process_exit](node::Environment* env, int exit_code) {
        MOMO_LOG(INFO) << "process.exit(" << exit_code << ")";
        if (on_exit) {
          on_exit(exit_code);
        }
        node::Stop(env);
      });
  // ui 线程独占本 isolate: 整个生命周期持锁并保持进入状态, 免去每次调用的 Locker / Isolate::Scope。
  runtime->locker_ = std::make_unique<v8::Locker>(isolate);
  isolate->Enter();
  isolate->SetData(kIsolateSlotRuntime, runtime.get());
  runtime->code_cache_ = !options.code_cache_dir.empty();

  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = runtime->setup_->context();
  runtime->context_.Reset(isolate, context);
  v8::Context::Scope context_scope(context);

  // uv 循环接到 ui 线程(Electron NodeBindings::PrepareEmbedThread → LoadEnvironment → StartPolling)。
  runtime->bindings_ =
      std::make_unique<NodeBindings>(runtime->env_, runtime->setup_->event_loop(), std::move(task_runner));
  runtime->bindings_->PrepareEmbedThread();

  JsRuntime* raw = runtime.get();
  const std::string cache_dir = options.code_cache_dir.string();
  v8::TryCatch try_catch(isolate);
  if (node::LoadEnvironment(
          runtime->env_,
          // 入口不在这里执行: 由 RunModule 经 Node 的 ESM 加载器执行(见 node_bootstrap.cc)。
          [isolate](const node::StartExecutionCallbackInfo&) -> v8::MaybeLocal<v8::Value> {
            return v8::Undefined(isolate);
          },
          [raw, cache_dir](node::Environment*, v8::Local<v8::Value> process,
                           v8::Local<v8::Value> require) {
            raw->RunBootstrap(process, require, cache_dir);
          })
          .IsEmpty() ||
      runtime->host_.IsEmpty()) {
    return try_catch.HasCaught() ? runtime->ExceptionToStatus(try_catch, "Node.js bootstrap")
                                 : InternalError("Node.js bootstrap failed");
  }
  runtime->bindings_->StartPolling();
  return runtime;
}

void JsRuntime::RunBootstrap(v8::Local<v8::Value> process, v8::Local<v8::Value> require,
                                 const std::string& code_cache_dir) {
  v8::Isolate* isolate = isolate_;
  v8::Local<v8::Context> context = this->context();
  v8::Local<v8::String> params[] = {V8String(isolate, "process"), V8String(isolate, "require"),
                                    V8String(isolate, "codeCacheDir")};
  v8::ScriptOrigin origin(V8String(isolate, "momo:bootstrap"));
  v8::ScriptCompiler::Source source(V8String(isolate, kNodeBootstrapSource), origin);
  v8::Local<v8::Function> fn;
  if (!v8::ScriptCompiler::CompileFunction(context, &source, 3, params).ToLocal(&fn)) {
    return;  // 异常留给 LoadEnvironment 的调用方
  }
  v8::Local<v8::Value> args[] = {process, require,
                                 code_cache_dir.empty() ? v8::Local<v8::Value>(v8::Null(isolate))
                                                        : V8String(isolate, code_cache_dir)};
  v8::Local<v8::Value> host;
  if (fn->Call(context, v8::Null(isolate), 3, args).ToLocal(&host) && host->IsObject()) {
    host_.Reset(isolate, host.As<v8::Object>());
  }
}

JsRuntime::~JsRuntime() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (isolate_ == nullptr) {
    return;
  }
  {
    v8::HandleScope handle_scope(isolate_);
    // 仍被 JS 持有的宿主对象: isolate 销毁时弱回调不会触发, 在此统一释放。
    while (!wrappables_.empty()) {
      delete *wrappables_.begin();  // 析构会 Untrack
    }
    class_templates_.clear();
    host_.Reset();
    context_.Reset();
  }
  // 先停 poll 线程(其保活句柄在 FreeEnvironment 运行循环时完成关闭), 再拆环境。
  bindings_->Stop();
  isolate_->SetData(kIsolateSlotRuntime, nullptr);
  isolate_->Exit();
  locker_.reset();
  // FreeEnvironment → FreeIsolateData → 注销并销毁 isolate → 关闭 uv 循环(CommonEnvironmentSetup 析构)。
  setup_.reset();
  bindings_.reset();
  isolate_ = nullptr;
}

JsRuntime* JsRuntime::From(v8::Isolate* isolate) {
  return static_cast<JsRuntime*>(isolate->GetData(kIsolateSlotRuntime));
}

// ---------------------------------------------------------------------------
// 异常

Status JsRuntime::ExceptionToStatus(v8::TryCatch& try_catch, std::string_view what) {
  v8::Isolate* isolate = isolate_;
  v8::Local<v8::Context> context = this->context();
  std::string message = std::string(what) + ": ";
  if (!try_catch.HasCaught()) {
    return InternalError(message + "unknown error");
  }
  if (try_catch.HasTerminated()) {
    return CancelledError(message + "execution terminated");
  }
  v8::Local<v8::Value> exception = try_catch.Exception();
  v8::Local<v8::Value> stack;
  if (try_catch.StackTrace(context).ToLocal(&stack) && stack->IsString()) {
    message += ToUtf8(isolate, stack);  // 含 "Error: msg" 与各帧
  } else {
    message += ToUtf8(isolate, exception);
    v8::Local<v8::Message> msg = try_catch.Message();
    if (!msg.IsEmpty()) {
      message += " (" + ToUtf8(isolate, msg->GetScriptResourceName()) + ":" +
                 std::to_string(msg->GetLineNumber(context).FromMaybe(0)) + ")";
    }
  }
  return InternalError(message);
}

StatusOr<v8::Local<v8::Value>> JsRuntime::Call(v8::Local<v8::Function> fn,
                                               v8::Local<v8::Value> receiver, int argc,
                                               v8::Local<v8::Value> argv[], std::string_view what) {
  v8::EscapableHandleScope scope(isolate_);
  // 与 node::MakeCallback 相同: 在 CallbackScope 里调用(异步上下文正确), 最外层的 scope 关闭时按 Node 语义
  // 排空 process.nextTick 与 microtask——从 momo 进入 JS 的每次调用都是一次完整的"宏任务"。
  // 调用本身已在 libuv 的顺序里: 入口都是 ui 线程上的任务, 与 uv 回调同属一个循环, 不另经 uv 排队。
  node::CallbackScope callback_scope(env_, v8::Object::New(isolate_), {0, 0});
  v8::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> result;
  if (!fn->Call(context(), receiver, argc, argv).ToLocal(&result)) {
    Status status = ExceptionToStatus(try_catch, what);
    MOMO_LOG(ERROR) << status;
    return status;
  }
  return scope.Escape(result);
}

void JsRuntime::DrainMicrotasks() {
  v8::HandleScope handle_scope(isolate_);
  v8::Context::Scope context_scope(context());
  {
    // 空的 CallbackScope 在关闭时按 Node 语义排空: process.nextTick 队列与 microtask(含未处理拒绝的处理)。
    node::CallbackScope scope(env_, v8::Object::New(isolate_), {0, 0});
  }
  // 刚执行的 JS 可能新增了定时器/IO: 让 poll 线程按新状态重新等待(见 node_bindings.h)。
  bindings_->WakeupEmbedThread();
}

// ---------------------------------------------------------------------------
// 经典脚本

StatusOr<std::string> JsRuntime::EvalToString(std::string_view source, std::string_view url) {
  v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = this->context();
  v8::Context::Scope context_scope(context);
  v8::TryCatch try_catch(isolate_);
  v8::ScriptOrigin origin(V8String(isolate_, url));
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> result;
  if (!v8::Script::Compile(context, V8String(isolate_, source), &origin).ToLocal(&script) ||
      !script->Run(context).ToLocal(&result)) {
    return ExceptionToStatus(try_catch, std::string("eval ") + std::string(url));
  }
  std::string out = ToUtf8(isolate_, result);
  DrainMicrotasks();
  return out;
}

// ---------------------------------------------------------------------------
// 模块

void JsRuntime::RegisterNativeModule(std::string specifier, NativeModuleFactory factory) {
  auto [it, inserted] = native_factories_.insert_or_assign(std::move(specifier), std::move(factory));
  if (inserted) {
    // 绑定在 import 时(process._linkedBinding)才初始化;Node 持有名字与 priv 指针, 二者在 map 中地址稳定。
    node::AddLinkedBinding(env_, it->first.c_str(), &JsRuntime::InitLinkedBinding, &*it);
  }
}

// static
void JsRuntime::InitLinkedBinding(v8::Local<v8::Object> exports, v8::Local<v8::Value>,
                                      v8::Local<v8::Context> context, void* priv) {
  auto* entry = static_cast<std::pair<const std::string, NativeModuleFactory>*>(priv);
  entry->second(v8::Isolate::GetCurrent(), context, exports);
}

Status JsRuntime::PromiseToStatus(v8::Local<v8::Promise> promise, std::string_view what) {
  v8::TryCatch try_catch(isolate_);
  isolate_->ThrowException(promise->Result());
  return ExceptionToStatus(try_catch, what);
}

Status JsRuntime::RunMainModule(const std::filesystem::path& path) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec).lexically_normal();
  if (!std::filesystem::is_regular_file(absolute, ec)) {
    return NotFoundError("JS bundle is not readable: " + absolute.string());
  }
  return RunModule(absolute.string(), nullptr, absolute.parent_path());
}

Status JsRuntime::RunModuleSource(std::string_view source, std::string_view url,
                                  const std::filesystem::path& base_dir) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  const std::string text(source);
  return RunModule(std::string(url), &text, base_dir);
}

Status JsRuntime::RunModule(const std::string& url, const std::string* source,
                            const std::filesystem::path& base_dir) {
  v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = this->context();
  v8::Context::Scope context_scope(context);
  v8::TryCatch try_catch(isolate_);
  const std::string what = "run module " + url;

  v8::Local<v8::Object> host = host_.Get(isolate_);
  v8::Local<v8::Value> import_fn;
  if (!host->Get(context, V8String(isolate_, "importModule")).ToLocal(&import_fn) ||
      !import_fn->IsFunction()) {
    return InternalError("Node.js bootstrap host is missing importModule");
  }
  v8::Local<v8::Value> args[] = {
      V8String(isolate_, url),
      source != nullptr ? v8::Local<v8::Value>(V8String(isolate_, *source))
                        : v8::Local<v8::Value>(v8::Undefined(isolate_)),
      V8String(isolate_, base_dir.string())};
  v8::Local<v8::Value> result;
  v8::Local<v8::Value> promise_value;
  if (!import_fn.As<v8::Function>()->Call(context, host, 3, args).ToLocal(&result) || !result->IsObject() ||
      !result.As<v8::Object>()->Get(context, V8String(isolate_, "promise")).ToLocal(&promise_value) ||
      !promise_value->IsPromise()) {
    return try_catch.HasCaught() ? ExceptionToStatus(try_catch, what)
                                 : InternalError(what + ": importModule did not return { promise, … }");
  }
  v8::Local<v8::Object> run = result.As<v8::Object>();
  v8::Local<v8::Promise> promise = promise_value.As<v8::Promise>();
  auto started = [&] {
    v8::Local<v8::Value> flag;
    return run->Get(context, V8String(isolate_, "started")).ToLocal(&flag) && flag->IsTrue();
  };

  // 模块图的加载经 uv_run 推进(同 Electron NodeBindings::JoinAppCode;文件读取在 uv 线程池完成), 到入口
  // 开始求值为止。入口的同步部分随即执行完;它若停在顶层 await, 不等待(与 Electron 的入口一致): 这里
  // 泵送的只有 uv, 本线程的引擎任务(平台通道应答、帧)不会执行, 等下去会卡住启动。余下的求值在正常的
  // 事件循环里继续, 之后的失败由 bootstrap 记录。保活句柄不计入存活: uv 无事可做时同样停止泵送。
  uv_loop_t* loop = bindings_->loop();
  bindings_->SetKeepAlive(false);
  while (true) {
    DrainMicrotasks();
    if (promise->State() != v8::Promise::kPending || started() || !uv_loop_alive(loop)) {
      break;
    }
    uv_run(loop, UV_RUN_ONCE);
  }
  bindings_->SetKeepAlive(true);

  if (promise->State() == v8::Promise::kRejected) {
    return PromiseToStatus(promise, what);
  }
  if (promise->State() == v8::Promise::kPending) {
    (void)run->Set(context, V8String(isolate_, "detached"), v8::True(isolate_));
    MOMO_LOG(INFO) << url << ": the entry module is suspended at a top-level await; startup does not "
                             "wait for it (evaluation continues asynchronously).";
  }
  if (code_cache_) {
    v8::Local<v8::Value> flush;
    if (host->Get(context, V8String(isolate_, "flushCompileCache")).ToLocal(&flush) &&
        flush->IsFunction()) {
      (void)flush.As<v8::Function>()->Call(context, host, 0, nullptr);
    }
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// 宿主类模板

v8::Local<v8::FunctionTemplate> JsRuntime::GetClassTemplate(const WrapperTypeInfo* info) {
  auto it = class_templates_.find(info);
  MOMO_CHECK(it != class_templates_.end()) << "class template not built: " << info->class_name;
  return it->second.Get(isolate_);
}

void JsRuntime::SetClassTemplate(const WrapperTypeInfo* info,
                                     v8::Local<v8::FunctionTemplate> tmpl) {
  class_templates_[info].Reset(isolate_, tmpl);
}

}  // namespace momo::engine::runtime
