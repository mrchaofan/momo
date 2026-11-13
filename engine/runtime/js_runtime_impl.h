#pragma once

// JsRuntime 的 V8 / Node 实现细节。只能被 engine/runtime 与 engine/ui 的实现文件包含(docs/10 §1)。
//
// 一个 JsRuntimeImpl = 一个 Node.js 环境(node::CommonEnvironmentSetup: uv 循环 + isolate + IsolateData +
// Environment), 运行在 ui 线程, 整个生命周期持有 v8::Locker 并保持 isolate 进入状态。
//   - 事件循环: NodeBindings 把 libuv 接到 ui 线程(见 node_bindings.h, 对照 Electron)。
//   - 模块: Node 的 ESM 加载器;momo:* / ext:* 经 module.registerHooks 映射到 node::AddLinkedBinding
//     注册的原生绑定(node_bootstrap.cc)。入口模块经 uv_run 泵送直到求值完成(或 uv 无事可做)。
//   - 从 momo 进入 JS(帧、输入、通道回调): Call 在 node::CallbackScope 里调用(同 node::MakeCallback),
//     返回时按 Node 语义排空 process.nextTick 与 microtask;DrainMicrotasks 另外唤醒 poll 线程。

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

namespace node {
class CommonEnvironmentSetup;
class Environment;
}  // namespace node

namespace momo::engine::runtime {

class NodeBindings;
class Wrappable;
struct WrapperTypeInfo;

// 原生模块导出工厂: 往 exports 上挂属性。在模块首次被 import 时于 ui 线程调用一次。
using NativeModuleFactory =
    std::function<void(v8::Isolate* isolate, v8::Local<v8::Context> context,
                       v8::Local<v8::Object> exports)>;

class JsRuntimeImpl {
 public:
  struct Options {
    // Node 编译缓存目录(module.enableCompileCache;空 = 不缓存)。
    std::filesystem::path code_cache_dir;
  };

  // task_runner: isolate 归属线程(ui)的 runner, uv 事件在这里处理;须在该线程调用。
  // node_args: 进程级的 Node 选项(如 --inspect=...), 只在进程内第一次创建时生效。
  static StatusOr<std::unique_ptr<JsRuntimeImpl>> Create(std::shared_ptr<TaskRunner> task_runner,
                                                         const Options& options,
                                                         const std::vector<std::string>& node_args);
  ~JsRuntimeImpl();

  static JsRuntimeImpl* From(v8::Isolate* isolate);

  v8::Isolate* isolate() const { return isolate_; }
  v8::Local<v8::Context> context() const { return context_.Get(isolate_); }
  node::Environment* env() const { return env_; }

  // 注册 `specifier`(如 "momo:ui")对应的原生模块(node::AddLinkedBinding)。须在执行导入它的模块之前注册。
  void RegisterNativeModule(std::string specifier, NativeModuleFactory factory);

  // 执行 ESM 模块并泵送 uv 循环, 直到求值完成或 uv 无事可做(此时求值继续异步进行, 返回 Ok)。
  // source 为空时 url 是文件路径;否则 url 是源码的名字, 相对导入基于 base_dir(空 = 当前目录)。
  Status RunModule(const std::string& url, const std::string* source,
                   const std::filesystem::path& base_dir);
  StatusOr<std::string> EvalToString(std::string_view source, std::string_view url);
  // 从 momo 进入 JS 之后调用: 按 Node 语义排空 nextTick 与 microtask, 并让 uv poll 线程按新状态等待。
  void DrainMicrotasks();

  // 调用 JS 函数(在本 context 中, 于 node::CallbackScope 内), 异常转为 Status 并记录日志。最外层的调用返回前
  // 按 Node 语义排空 nextTick 与 microtask(嵌套调用由最外层排空)。
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
  JsRuntimeImpl();

  // node::AddLinkedBinding 的初始化回调(priv 指向 native_factories_ 中的条目)。
  static void InitLinkedBinding(v8::Local<v8::Object> exports, v8::Local<v8::Value> module,
                                v8::Local<v8::Context> context, void* priv);
  // Node preload: 执行 node_bootstrap, 保存宿主对象。
  void RunBootstrap(v8::Local<v8::Value> process, v8::Local<v8::Value> require,
                    const std::string& code_cache_dir);
  Status PromiseToStatus(v8::Local<v8::Promise> promise, std::string_view what);

  std::unique_ptr<node::CommonEnvironmentSetup> setup_;
  std::unique_ptr<v8::Locker> locker_;
  v8::Isolate* isolate_ = nullptr;
  node::Environment* env_ = nullptr;
  v8::Global<v8::Context> context_;
  std::unique_ptr<NodeBindings> bindings_;
  // node_bootstrap 返回的宿主对象: { importModule, flushCompileCache }。
  v8::Global<v8::Object> host_;
  bool code_cache_ = false;

  // 原生模块: specifier → 工厂。std::map 的节点地址稳定, 键的 c_str() 与条目地址交给 Node 长期持有。
  std::map<std::string, NativeModuleFactory> native_factories_;

  std::unordered_map<const WrapperTypeInfo*, v8::Global<v8::FunctionTemplate>> class_templates_;
  std::unordered_set<Wrappable*> wrappables_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(JsRuntimeImpl);
};

}  // namespace momo::engine::runtime
