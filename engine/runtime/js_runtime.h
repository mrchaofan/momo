#pragma once

// JsRuntime: 一个 Node.js 环境(isolate + 主 Context + libuv 循环, docs/04 §1)。公共头不含 V8/Node 类型,
// shell 只通过它驱动 JS;绑定代码(runtime/ui 实现文件)经 impl() 访问 V8 细节(js_runtime_impl.h)。
// 线程约定: momo::Object, 归属 ui 线程, 仅在该线程创建、调用与析构;libuv 事件也在该线程处理
// (node_bindings.h)。
//
// 模块: 主程序为 ESM, 由 Node 的 ESM 加载器执行: 可以 import `node:*`、npm 包(node_modules), 以及
// `momo:*` / `ext:*` 原生模块(RegisterNativeModule 注册)。每次从 momo 进入 JS 后由调用方
// DrainMicrotasks(按 Node 语义排空 nextTick 与 microtask)。

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"

namespace momo::engine::runtime {

class JsRuntimeImpl;

class JsRuntime final : public Object {
 public:
  struct Options {
    // Node 编译缓存目录(module.enableCompileCache;空 = 不缓存)。
    std::filesystem::path code_cache_dir;
    // Node inspector(Chrome DevTools)。进程级: 只在进程内第一次创建运行时时生效。
    bool enable_inspector = false;
    int inspector_port = 9229;      // 0 = 系统分配
    bool inspector_wait = false;    // 执行 bundle 前等待调试器连接(--inspect-wait)
  };

  static StatusOr<std::unique_ptr<JsRuntime>> Create(const Options& options);
  ~JsRuntime() override;

  // 加载并执行 ESM 主模块(含其静态依赖), 期间泵送 uv 循环直到求值完成(或 uv 无事可做, 此时顶层 await
  // 继续异步进行)。语法错误、解析失败、顶层抛出/拒绝都返回错误(消息含 JS 栈)。
  Status RunMainModule(const std::filesystem::path& path);
  // 以 source 作为 ESM 模块执行;相对导入基于 base_dir。测试与 --dev 片段用。
  Status RunModuleSource(std::string_view source, std::string_view url,
                         const std::filesystem::path& base_dir = {});
  // 以经典脚本求值, 结果转为字符串。测试/诊断用。
  StatusOr<std::string> EvalToString(std::string_view source, std::string_view url = "<eval>");

  void DrainMicrotasks();

  JsRuntimeImpl& impl() { return *impl_; }

 private:
  explicit JsRuntime(std::unique_ptr<JsRuntimeImpl> impl);

  std::unique_ptr<JsRuntimeImpl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(JsRuntime);
};

}  // namespace momo::engine::runtime
