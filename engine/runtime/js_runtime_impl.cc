#include "engine/runtime/js_runtime_impl.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "engine/runtime/js_args.h"
#include "engine/runtime/v8_platform.h"
#include "engine/runtime/wrappable.h"

namespace momo::engine::runtime {

namespace {

constexpr uint32_t kIsolateSlotRuntime = 0;

// ---------------------------------------------------------------------------
// V8 进程级初始化: V8 要求每进程恰好一次, 且不能在 Dispose 后重新初始化。
// 这是 docs/10「禁全局可变状态」的必要例外: 只在这里、只写一次、永不释放。

struct V8Process {
  bool icu_ok = false;
};

V8Process& InitializeV8Once(const std::filesystem::path& icu_data) {
  static std::once_flag once;
  static V8Process* process = nullptr;  // 有意泄漏: 进程退出前 V8 不可 Dispose
  std::call_once(once, [&icu_data] {
    process = new V8Process();
    std::filesystem::path icu = icu_data;
    std::error_code ec;
#ifdef MOMO_V8_ICU_DATA
    if (icu.empty() || !std::filesystem::is_regular_file(icu, ec)) {
      icu = MOMO_V8_ICU_DATA;  // 构建树内的预编译包(开发/测试)
    }
#endif
    process->icu_ok = !icu.empty() && std::filesystem::is_regular_file(icu, ec) &&
                      v8::V8::InitializeICU(icu.string().c_str());
    if (!process->icu_ok) {
      MOMO_LOG(WARNING) << "V8 ICU data not loaded (" << icu.string() << "); Intl is unavailable.";
    }
    // 前台任务走 isolate 归属线程的 EventLoop, 后台任务走 base::WorkerPool(见 v8_platform.h)。
    v8::V8::InitializePlatform(&V8Platform::Get());
    MOMO_CHECK(v8::V8::Initialize()) << "V8 initialization failed";
    MOMO_LOG(INFO) << "V8 " << v8::V8::GetVersion() << " initialized.";
  });
  return *process;
}

std::string ReadFile(const std::filesystem::path& path, bool* ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *ok = false;
    return {};
  }
  std::stringstream ss;
  ss << in.rdbuf();
  *ok = true;
  return ss.str();
}

// ---- console ----

void ConsoleLog(const Args& args, LogSeverity severity) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  std::string line;
  for (int i = 0; i < args.Length(); ++i) {
    if (i > 0) {
      line += ' ';
    }
    v8::Local<v8::Value> v = args[i];
    if (v->IsObject() && !v->IsFunction() && !v->IsNativeError()) {
      v8::Local<v8::String> json;
      if (v8::JSON::Stringify(context, v).ToLocal(&json)) {
        line += ToUtf8(isolate, json);
        continue;
      }
    }
    line += ToUtf8(isolate, v);
  }
  if (::momo::LogMessage::ShouldLog(severity)) {
    ::momo::LogMessage(severity, "js", 0, nullptr).stream() << line;
  }
  // 转发给 V8 内建的同名 console 方法(函数 data): 挂了 Inspector 时由它把消息(含调用栈)送到
  // DevTools;没有 Inspector 时内建实现是空操作。
  v8::Local<v8::Value> builtin = args.Data();
  if (!builtin.IsEmpty() && builtin->IsFunction()) {
    std::vector<v8::Local<v8::Value>> argv;
    argv.reserve(static_cast<size_t>(args.Length()));
    for (int i = 0; i < args.Length(); ++i) {
      argv.push_back(args[i]);
    }
    v8::TryCatch try_catch(isolate);
    (void)builtin.As<v8::Function>()->Call(context, args.This(), args.Length(), argv.data());
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// 创建与销毁

StatusOr<std::unique_ptr<JsRuntimeImpl>> JsRuntimeImpl::Create(
    const std::filesystem::path& icu_data, std::shared_ptr<TaskRunner> task_runner) {
  MOMO_CHECK(task_runner != nullptr);
  InitializeV8Once(icu_data);
  std::unique_ptr<JsRuntimeImpl> impl(new JsRuntimeImpl());
  impl->allocator_ = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = impl->allocator_;
  // 先分配再注册前台 runner, 最后初始化: V8 在 Initialize 期间就会取前台 runner。
  v8::Isolate* isolate = v8::Isolate::Allocate();
  if (isolate == nullptr) {
    return InternalError("v8::Isolate::Allocate failed");
  }
  JsRuntimeImpl* raw = impl.get();
  V8Platform::Get().RegisterIsolate(isolate, std::move(task_runner),
                                    [raw] { raw->DrainMicrotasks(); });
  impl->isolate_ = isolate;
  v8::Isolate::Initialize(isolate, params);
  isolate->SetData(kIsolateSlotRuntime, impl.get());
  isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  isolate->SetPromiseRejectCallback(&JsRuntimeImpl::OnPromiseReject);
  // ui 线程独占本 isolate: 整个生命周期保持进入状态, 免去每次调用的 Isolate::Scope。
  isolate->Enter();

  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = v8::Context::New(isolate);
  impl->context_.Reset(isolate, context);
  v8::Context::Scope context_scope(context);  // Object::New 等需要当前 context
  impl->InstallConsole(context);
  return impl;
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
    synthetic_exports_.clear();
    module_urls_.clear();
    modules_.clear();
    class_templates_.clear();
    context_.Reset();
  }
  isolate_->Exit();
  isolate_->Dispose();
  // Dispose 期间 V8 仍可能取前台 runner, 所以之后才注销;尚未执行的前台任务随之作废。
  V8Platform::Get().UnregisterIsolate(isolate_);
  isolate_ = nullptr;
  delete allocator_;
}

JsRuntimeImpl* JsRuntimeImpl::From(v8::Isolate* isolate) {
  return static_cast<JsRuntimeImpl*>(isolate->GetData(kIsolateSlotRuntime));
}

void JsRuntimeImpl::InstallConsole(v8::Local<v8::Context> context) {
  v8::Isolate* isolate = isolate_;
  // 就地覆盖 V8 内建 console 的常用方法(其余如 table/time/group 保留内建实现), 原方法作为函数
  // data 保存并在写完引擎日志后转发 —— Inspector(engine/runtime/inspector.h)靠内建实现接收 console 消息。
  v8::Local<v8::Object> console;
  v8::Local<v8::Value> existing;
  if (context->Global()->Get(context, V8String(isolate, "console")).ToLocal(&existing) &&
      existing->IsObject()) {
    console = existing.As<v8::Object>();
  } else {
    console = v8::Object::New(isolate);
  }
  auto add = [&](const char* name, v8::FunctionCallback cb) {
    v8::Local<v8::String> key = V8String(isolate, name);
    v8::Local<v8::Value> builtin;
    if (!console->Get(context, key).ToLocal(&builtin) || !builtin->IsFunction()) {
      builtin = v8::Undefined(isolate);
    }
    console->Set(context, key, v8::Function::New(context, cb, builtin).ToLocalChecked()).Check();
  };
  add("log", [](const Args& a) { ConsoleLog(a, LogSeverity::kInfo); });
  add("info", [](const Args& a) { ConsoleLog(a, LogSeverity::kInfo); });
  add("debug", [](const Args& a) { ConsoleLog(a, LogSeverity::kVerbose); });
  add("warn", [](const Args& a) { ConsoleLog(a, LogSeverity::kWarning); });
  add("error", [](const Args& a) { ConsoleLog(a, LogSeverity::kError); });
  context->Global()->Set(context, V8String(isolate, "console"), console).Check();
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

void JsRuntimeImpl::OnPromiseReject(v8::PromiseRejectMessage message) {
  if (message.GetEvent() != v8::kPromiseRejectWithNoHandler) {
    return;
  }
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::Local<v8::Value> value = message.GetValue();
  std::string text = value.IsEmpty() ? "undefined" : ToUtf8(isolate, value);
  if (!value.IsEmpty() && value->IsNativeError()) {
    v8::Local<v8::Value> stack;
    if (value.As<v8::Object>()
            ->Get(isolate->GetCurrentContext(), V8String(isolate, "stack"))
            .ToLocal(&stack) &&
        stack->IsString()) {
      text = ToUtf8(isolate, stack);
    }
  }
  MOMO_LOG(ERROR) << "Unhandled promise rejection: " << text;
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

void JsRuntimeImpl::DrainMicrotasks() { isolate_->PerformMicrotaskCheckpoint(); }

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
  native_factories_[std::move(specifier)] = std::move(factory);
}

std::filesystem::path JsRuntimeImpl::CodeCachePath(std::string_view source) const {
  uint64_t h = 1469598103934665603ULL;  // FNV-1a(源码) ^ V8 版本/flag 标签
  for (const char c : source) {
    h ^= static_cast<uint8_t>(c);
    h *= 1099511628211ULL;
  }
  h ^= static_cast<uint64_t>(v8::ScriptCompiler::CachedDataVersionTag()) << 1;
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.v8c", static_cast<unsigned long long>(h));
  return code_cache_dir_ / name;
}

v8::MaybeLocal<v8::Module> JsRuntimeImpl::CompileModule(std::string_view source,
                                                        const std::string& url) {
  v8::EscapableHandleScope scope(isolate_);
  v8::ScriptOrigin origin(V8String(isolate_, url), 0, 0, false, -1, v8::Local<v8::Value>(), false,
                          false, /*is_module=*/true);
  // 只缓存文件模块(绝对路径), 测试/eval 片段不缓存。
  const bool cacheable = !code_cache_dir_.empty() && std::filesystem::path(url).is_absolute();
  std::filesystem::path cache_path;
  v8::ScriptCompiler::CachedData* cached = nullptr;
  if (cacheable) {
    cache_path = CodeCachePath(source);
    bool ok = false;
    const std::string bytes = ReadFile(cache_path, &ok);
    if (ok && !bytes.empty()) {
      auto* buffer = new uint8_t[bytes.size()];  // 所有权交给 CachedData(BufferOwned)
      std::memcpy(buffer, bytes.data(), bytes.size());
      cached = new v8::ScriptCompiler::CachedData(buffer, static_cast<int>(bytes.size()),
                                                   v8::ScriptCompiler::CachedData::BufferOwned);
    }
  }
  // Source 析构时释放 cached。
  v8::ScriptCompiler::Source script_source(V8String(isolate_, source), origin, cached);
  v8::Local<v8::Module> module;
  const auto options = cached != nullptr ? v8::ScriptCompiler::kConsumeCodeCache
                                         : v8::ScriptCompiler::kNoCompileOptions;
  if (!v8::ScriptCompiler::CompileModule(isolate_, &script_source, options).ToLocal(&module)) {
    return {};
  }
  if (cacheable) {
    if (cached != nullptr && !script_source.GetCachedData()->rejected) {
      code_cache_stats_.hits++;
    } else {
      if (cached != nullptr) {
        code_cache_stats_.rejected++;
      }
      pending_code_caches_.emplace_back(v8::Global<v8::Module>(isolate_, module), cache_path);
    }
  }
  module_urls_.emplace(module->GetIdentityHash(),
                       std::make_pair(v8::Global<v8::Module>(isolate_, module), url));
  return scope.Escape(module);
}

v8::MaybeLocal<v8::Module> JsRuntimeImpl::LoadFileModule(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  const std::string key = (ec ? path : canonical).string();
  if (auto it = modules_.find(key); it != modules_.end()) {
    return it->second.Get(isolate_);
  }
  bool ok = false;
  const std::string source = ReadFile(key, &ok);
  if (!ok) {
    ThrowError(isolate_, "Cannot find module '" + key + "'");
    return {};
  }
  v8::Local<v8::Module> module;
  if (!CompileModule(source, key).ToLocal(&module)) {
    return {};
  }
  modules_.emplace(key, v8::Global<v8::Module>(isolate_, module));
  return module;
}

v8::MaybeLocal<v8::Module> JsRuntimeImpl::LoadNativeModule(const std::string& specifier) {
  if (auto it = modules_.find(specifier); it != modules_.end()) {
    return it->second.Get(isolate_);
  }
  auto factory = native_factories_.find(specifier);
  if (factory == native_factories_.end()) {
    ThrowError(isolate_, "Unknown native module '" + specifier + "'");
    return {};
  }
  v8::EscapableHandleScope scope(isolate_);
  v8::Local<v8::Context> context = this->context();
  v8::Local<v8::Object> exports = v8::Object::New(isolate_);
  factory->second(isolate_, context, exports);

  v8::Local<v8::Array> keys = exports->GetOwnPropertyNames(context).ToLocalChecked();
  std::vector<v8::Local<v8::String>> names;
  for (uint32_t i = 0; i < keys->Length(); ++i) {
    names.push_back(keys->Get(context, i).ToLocalChecked().As<v8::String>());
  }
  v8::Local<v8::Module> module = v8::Module::CreateSyntheticModule(
      isolate_, V8String(isolate_, specifier),
      std::span<const v8::Local<v8::String>>(names.data(), names.size()),
      &JsRuntimeImpl::EvaluateSyntheticModule);
  synthetic_exports_.emplace(module->GetIdentityHash(),
                             std::make_pair(v8::Global<v8::Module>(isolate_, module),
                                            v8::Global<v8::Object>(isolate_, exports)));
  modules_.emplace(specifier, v8::Global<v8::Module>(isolate_, module));
  return scope.Escape(module);
}

v8::MaybeLocal<v8::Promise> JsRuntimeImpl::EvaluateSyntheticModule(v8::Local<v8::Context> context,
                                                                   v8::Local<v8::Module> module) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  JsRuntimeImpl* self = From(isolate);
  auto [begin, end] = self->synthetic_exports_.equal_range(module->GetIdentityHash());
  for (auto it = begin; it != end; ++it) {
    if (it->second.first.Get(isolate) != module) {
      continue;
    }
    v8::Local<v8::Object> exports = it->second.second.Get(isolate);
    v8::Local<v8::Array> keys = exports->GetOwnPropertyNames(context).ToLocalChecked();
    for (uint32_t i = 0; i < keys->Length(); ++i) {
      v8::Local<v8::String> name = keys->Get(context, i).ToLocalChecked().As<v8::String>();
      v8::Local<v8::Value> value = exports->Get(context, name).ToLocalChecked();
      if (module->SetSyntheticModuleExport(isolate, name, value).IsNothing()) {
        return {};
      }
    }
    break;
  }
  v8::Local<v8::Promise::Resolver> resolver = v8::Promise::Resolver::New(context).ToLocalChecked();
  resolver->Resolve(context, v8::Undefined(isolate)).Check();
  return resolver->GetPromise();
}

v8::MaybeLocal<v8::Module> JsRuntimeImpl::ResolveModule(v8::Local<v8::Context> context,
                                                        v8::Local<v8::String> specifier,
                                                        v8::Local<v8::FixedArray> /*attributes*/,
                                                        v8::Local<v8::Module> referrer) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  JsRuntimeImpl* self = From(isolate);
  const std::string spec = ToUtf8(isolate, specifier);
  if (spec.rfind("momo:", 0) == 0 || spec.rfind("ext:", 0) == 0) {
    return self->LoadNativeModule(spec);
  }
  // 文件模块: 相对路径基于引用方所在目录(引用方为源码片段时用 current_base_dir_)。
  std::filesystem::path base = self->current_base_dir_;
  auto [begin, end] = self->module_urls_.equal_range(referrer->GetIdentityHash());
  for (auto it = begin; it != end; ++it) {
    if (it->second.first.Get(isolate) == referrer) {
      const std::filesystem::path referrer_path(it->second.second);
      if (referrer_path.is_absolute()) {
        base = referrer_path.parent_path();
      }
      break;
    }
  }
  std::filesystem::path target(spec);
  if (spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0) {
    target = base / target;
  } else if (!target.is_absolute()) {
    ThrowError(isolate, "Cannot resolve module '" + spec +
                            "' (only momo:*, ext:*, relative and absolute paths are supported)");
    return {};
  }
  return self->LoadFileModule(target.lexically_normal());
}

Status JsRuntimeImpl::RunModule(std::string_view source, const std::string& url,
                                const std::filesystem::path& base_dir) {
  v8::HandleScope handle_scope(isolate_);
  v8::Local<v8::Context> context = this->context();
  v8::Context::Scope context_scope(context);
  v8::TryCatch try_catch(isolate_);
  current_base_dir_ = base_dir;

  v8::Local<v8::Module> module;
  if (!CompileModule(source, url).ToLocal(&module)) {
    return ExceptionToStatus(try_catch, "compile " + url);
  }
  if (std::filesystem::path(url).is_absolute()) {
    modules_.emplace(url, v8::Global<v8::Module>(isolate_, module));
  }
  if (!module->InstantiateModule(context, &JsRuntimeImpl::ResolveModule).FromMaybe(false)) {
    return ExceptionToStatus(try_catch, "link " + url);
  }
  v8::Local<v8::Promise> promise;
  if (!module->Evaluate(context).ToLocal(&promise)) {
    return ExceptionToStatus(try_catch, "evaluate " + url);
  }
  DrainMicrotasks();
  if (promise->State() == v8::Promise::kRejected) {
    // 顶层抛出或拒绝: 用拒绝值构造错误。
    try_catch.Reset();
    isolate_->ThrowException(promise->Result());
    promise->MarkAsHandled();
    pending_code_caches_.clear();  // 出错的 bundle 不缓存
    return ExceptionToStatus(try_catch, "evaluate " + url);
  }
  FlushCodeCaches();
  return Status::Ok();
}

void JsRuntimeImpl::FlushCodeCaches() {
  if (pending_code_caches_.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(code_cache_dir_, ec);
  for (auto& [global, path] : pending_code_caches_) {
    v8::HandleScope scope(isolate_);
    v8::Local<v8::Module> module = global.Get(isolate_);
    std::unique_ptr<v8::ScriptCompiler::CachedData> data(
        v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript()));
    if (data == nullptr || data->length <= 0) {
      continue;
    }
    // 先写临时文件再改名: 并发启动的另一个进程不会读到半个文件。
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(data->data), data->length);
      if (!out) {
        continue;  // 缓存目录不可写: 静默放弃(只是失去优化)
      }
    }
    std::filesystem::rename(tmp, path, ec);
    if (!ec) {
      code_cache_stats_.written++;
    }
  }
  pending_code_caches_.clear();
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
