#pragma once

// JsRuntime 的 V8 实现细节。只能被 engine/runtime 与 engine/ui 的实现文件包含(docs/10 §1)。

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/macros.h"
#include "base/status.h"
#include "base/task_runner.h"
#include "v8.h"

namespace momo::engine::runtime {

class Wrappable;
struct WrapperTypeInfo;

// 原生模块导出工厂: 往 exports 上挂属性。在模块首次被 import 时于 ui 线程调用一次。
using NativeModuleFactory =
    std::function<void(v8::Isolate* isolate, v8::Local<v8::Context> context,
                       v8::Local<v8::Object> exports)>;

class JsRuntimeImpl {
 public:
  // task_runner: isolate 归属线程(ui)的 runner, V8 前台任务投递到这里;须在该线程调用。
  static StatusOr<std::unique_ptr<JsRuntimeImpl>> Create(const std::filesystem::path& icu_data,
                                                         std::shared_ptr<TaskRunner> task_runner);
  ~JsRuntimeImpl();

  static JsRuntimeImpl* From(v8::Isolate* isolate);

  v8::Isolate* isolate() const { return isolate_; }
  v8::Local<v8::Context> context() const { return context_.Get(isolate_); }

  // V8 code cache 目录(冷启动优化, docs/11 M6): 非空时文件模块的编译结果(字节码)缓存到该目录,
  // 下次以相同源码(按源码哈希 + V8 版本/flag 标签键控)启动时直接反序列化, 跳过解析与编译。
  // 缓存在模块首次执行完成后写入(此时已惰性编译的函数也包含在内)。损坏/不匹配的缓存被 V8 拒绝后自动重建。
  void SetCodeCacheDir(std::filesystem::path dir) { code_cache_dir_ = std::move(dir); }
  struct CodeCacheStats {
    int hits = 0;      // 使用缓存编译成功
    int rejected = 0;  // 缓存存在但被拒绝(版本/源码不匹配, 已重建)
    int written = 0;
  };
  const CodeCacheStats& code_cache_stats() const { return code_cache_stats_; }

  // 注册 `specifier`(如 "momo:ui")对应的原生模块。须在执行导入它的模块之前注册。
  void RegisterNativeModule(std::string specifier, NativeModuleFactory factory);

  Status RunModule(std::string_view source, const std::string& url,
                   const std::filesystem::path& base_dir);
  StatusOr<std::string> EvalToString(std::string_view source, std::string_view url);
  void DrainMicrotasks();

  // 调用 JS 函数(在本 context 中), 异常转为 Status 并记录日志。之后不自动排空 microtask。
  StatusOr<v8::Local<v8::Value>> Call(v8::Local<v8::Function> fn, v8::Local<v8::Value> receiver,
                                      int argc, v8::Local<v8::Value> argv[],
                                      std::string_view what);

  // TryCatch 中的异常 → Status(INTERNAL, 消息 + 位置 + 栈)。
  Status ExceptionToStatus(v8::TryCatch& try_catch, std::string_view what);

  // ---- 宿主对象(Wrappable)支持 ----
  v8::Local<v8::FunctionTemplate> GetClassTemplate(const WrapperTypeInfo* info);
  void SetClassTemplate(const WrapperTypeInfo* info, v8::Local<v8::FunctionTemplate> tmpl);
  void TrackWrappable(Wrappable* object) { wrappables_.insert(object); }
  void UntrackWrappable(Wrappable* object) { wrappables_.erase(object); }
  size_t live_wrappables() const { return wrappables_.size(); }

 private:
  JsRuntimeImpl() = default;

  static v8::MaybeLocal<v8::Module> ResolveModule(v8::Local<v8::Context> context,
                                                  v8::Local<v8::String> specifier,
                                                  v8::Local<v8::FixedArray> import_attributes,
                                                  v8::Local<v8::Module> referrer);
  static v8::MaybeLocal<v8::Promise> EvaluateSyntheticModule(v8::Local<v8::Context> context,
                                                             v8::Local<v8::Module> module);
  static void OnPromiseReject(v8::PromiseRejectMessage message);

  v8::MaybeLocal<v8::Module> LoadNativeModule(const std::string& specifier);
  v8::MaybeLocal<v8::Module> LoadFileModule(const std::filesystem::path& path);
  v8::MaybeLocal<v8::Module> CompileModule(std::string_view source, const std::string& url);
  void InstallConsole(v8::Local<v8::Context> context);
  std::filesystem::path CodeCachePath(std::string_view source) const;
  void FlushCodeCaches();

  v8::ArrayBuffer::Allocator* allocator_ = nullptr;
  v8::Isolate* isolate_ = nullptr;
  v8::Global<v8::Context> context_;

  // 模块缓存。key: 原生模块 specifier / 文件模块绝对路径。
  std::map<std::string, NativeModuleFactory> native_factories_;
  std::map<std::string, v8::Global<v8::Module>> modules_;
  // 模块 identity hash → 其 url/路径(用于解析相对导入)与 synthetic 导出对象。
  std::unordered_multimap<int, std::pair<v8::Global<v8::Module>, std::string>> module_urls_;
  std::unordered_multimap<int, std::pair<v8::Global<v8::Module>, v8::Global<v8::Object>>>
      synthetic_exports_;
  std::filesystem::path current_base_dir_;
  std::filesystem::path code_cache_dir_;
  CodeCacheStats code_cache_stats_;
  // 本次编译未命中缓存、待执行后写缓存的模块。
  std::vector<std::pair<v8::Global<v8::Module>, std::filesystem::path>> pending_code_caches_;

  std::unordered_map<const WrapperTypeInfo*, v8::Global<v8::FunctionTemplate>> class_templates_;
  std::unordered_set<Wrappable*> wrappables_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(JsRuntimeImpl);
};

}  // namespace momo::engine::runtime
