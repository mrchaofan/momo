#include "engine/runtime/js_runtime_impl.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

JsRuntimeImpl::JsRuntimeImpl() = default;

StatusOr<std::unique_ptr<JsRuntimeImpl>> JsRuntimeImpl::Create(
    std::shared_ptr<TaskRunner> task_runner, const Options& options,
    const std::vector<std::string>& node_args) {
  MOMO_CHECK(task_runner != nullptr);
  NodeProcess& process = InitializeNodeOnce(node_args);
  std::unique_ptr<JsRuntimeImpl> impl(new JsRuntimeImpl());

  std::vector<std::string> errors;
  impl->setup_ = node::CommonEnvironmentSetup::Create(
      process.result->platform(), &errors, process.result->args(), process.result->exec_args(),
      node::EnvironmentFlags::kDefaultFlags);
  if (impl->setup_ == nullptr) {
    std::string message = "failed to create the Node.js environment";
    for (const std::string& error : errors) {
      message += "; " + error;
    }
    return InternalError(message);
  }
  v8::Isolate* isolate = impl->setup_->isolate();
  impl->isolate_ = isolate;
  impl->env_ = impl->setup_->env();
  // ui 线程独占本 isolate: 整个生命周期持锁并保持进入状态, 免去每次调用的 Locker / Isolate::Scope。
  impl->locker_ = std::make_unique<v8::Locker>(isolate);
  isolate->Enter();
  isolate->SetData(kIsolateSlotRuntime, impl.get());
  impl->code_cache_ = !options.code_cache_dir.empty();

  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = impl->setup_->context();
  impl->context_.Reset(isolate, context);
  v8::Context::Scope context_scope(context);

  // uv 循环接到 ui 线程(Electron NodeBindings::PrepareEmbedThread → LoadEnvironment → StartPolling)。
  impl->bindings_ =
      std::make_unique<NodeBindings>(impl->env_, impl->setup_->event_loop(), std::move(task_runner));
  impl->bindings_->PrepareEmbedThread();

  JsRuntimeImpl* raw = impl.get();
  const std::string cache_dir = options.code_cache_dir.string();
  v8::TryCatch try_catch(isolate);
  if (node::LoadEnvironment(
          impl->env_,
          // 入口不在这里执行: 由 RunModule 经 Node 的 ESM 加载器执行(见 node_bootstrap.cc)。
          [isolate](const node::StartExecutionCallbackInfo&) -> v8::MaybeLocal<v8::Value> {
            return v8::Undefined(isolate);
          },
          [raw, cache_dir](node::Environment*, v8::Local<v8::Value> process,
                           v8::Local<v8::Value> require) {
            raw->RunBootstrap(process, require, cache_dir);
          })
          .IsEmpty() ||
      impl->host_.IsEmpty()) {
    return try_catch.HasCaught() ? impl->ExceptionToStatus(try_catch, "Node.js bootstrap")
                                 : InternalError("Node.js bootstrap failed");
  }
  impl->bindings_->StartPolling();
  return impl;
}

void JsRuntimeImpl::RunBootstrap(v8::Local<v8::Value> process, v8::Local<v8::Value> require,
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

JsRuntimeImpl::~JsRuntimeImpl() {
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

JsRuntimeImpl* JsRuntimeImpl::From(v8::Isolate* isolate) {
  return static_cast<JsRuntimeImpl*>(isolate->GetData(kIsolateSlotRuntime));
}

// ---------------------------------------------------------------------------
// 异常

Status JsRuntimeImpl::ExceptionToStatus(v8::TryCatch& try_catch, std::string_view what) {
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

StatusOr<v8::Local<v8::Value>> JsRuntimeImpl::Call(v8::Local<v8::Function> fn,
                                                   v8::Local<v8::Value> receiver, int argc,
                                                   v8::Local<v8::Value> argv[],
                                                   std::string_view what) {
  v8::EscapableHandleScope scope(isolate_);
  v8::TryCatch try_catch(isolate_);
  v8::Local<v8::Value> result;
  if (!fn->Call(context(), receiver, argc, argv).ToLocal(&result)) {
    Status status = ExceptionToStatus(try_catch, what);
    MOMO_LOG(ERROR) << status;
    return status;
  }
  return scope.Escape(result);
}

void JsRuntimeImpl::DrainMicrotasks() {
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

StatusOr<std::string> JsRuntimeImpl::EvalToString(std::string_view source, std::string_view url) {
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

void JsRuntimeImpl::RegisterNativeModule(std::string specifier, NativeModuleFactory factory) {
  auto [it, inserted] = native_factories_.insert_or_assign(std::move(specifier), std::move(factory));
  if (inserted) {
    // 绑定在 import 时(process._linkedBinding)才初始化;Node 持有名字与 priv 指针, 二者在 map 中地址稳定。
    node::AddLinkedBinding(env_, it->first.c_str(), &JsRuntimeImpl::InitLinkedBinding, &*it);
  }
}

// static
void JsRuntimeImpl::InitLinkedBinding(v8::Local<v8::Object> exports, v8::Local<v8::Value>,
                                      v8::Local<v8::Context> context, void* priv) {
  auto* entry = static_cast<std::pair<const std::string, NativeModuleFactory>*>(priv);
  entry->second(v8::Isolate::GetCurrent(), context, exports);
}

Status JsRuntimeImpl::PromiseToStatus(v8::Local<v8::Promise> promise, std::string_view what) {
  v8::TryCatch try_catch(isolate_);
  isolate_->ThrowException(promise->Result());
  return ExceptionToStatus(try_catch, what);
}

Status JsRuntimeImpl::RunModule(const std::string& url, const std::string* source,
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
  if (!import_fn.As<v8::Function>()->Call(context, host, 3, args).ToLocal(&result) ||
      !result->IsPromise()) {
    return try_catch.HasCaught() ? ExceptionToStatus(try_catch, what)
                                 : InternalError(what + ": importModule did not return a Promise");
  }
  v8::Local<v8::Promise> promise = result.As<v8::Promise>();

  // 入口经 uv_run 推进(同 Electron NodeBindings::JoinAppCode): 加载模块图(文件读取在 uv 线程池完成)、
  // 顶层 await 的 Node IO 都在这里完成。保活句柄不计入存活: uv 无事可做时停止泵送——剩下的求值
  // (如顶层 await 等待 momo 的平台通道)继续异步进行, 失败时作为未处理的拒绝记录。
  uv_loop_t* loop = bindings_->loop();
  bindings_->SetKeepAlive(false);
  while (true) {
    DrainMicrotasks();
    if (promise->State() != v8::Promise::kPending || !uv_loop_alive(loop)) {
      break;
    }
    uv_run(loop, UV_RUN_ONCE);
  }
  bindings_->SetKeepAlive(true);

  if (promise->State() == v8::Promise::kRejected) {
    return PromiseToStatus(promise, what);
  }
  if (promise->State() == v8::Promise::kPending) {
    MOMO_LOG(VERBOSE) << url << ": top-level await still pending; evaluation continues asynchronously.";
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

v8::Local<v8::FunctionTemplate> JsRuntimeImpl::GetClassTemplate(const WrapperTypeInfo* info) {
  auto it = class_templates_.find(info);
  MOMO_CHECK(it != class_templates_.end()) << "class template not built: " << info->class_name;
  return it->second.Get(isolate_);
}

void JsRuntimeImpl::SetClassTemplate(const WrapperTypeInfo* info,
                                     v8::Local<v8::FunctionTemplate> tmpl) {
  class_templates_[info].Reset(isolate_, tmpl);
}

}  // namespace momo::engine::runtime
