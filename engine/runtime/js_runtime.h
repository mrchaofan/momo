#pragma once

// JsRuntime: 一个 V8 Isolate + 主 Context(docs/04 §1)。公共头不含 V8 类型, shell 只通过它驱动 JS;
// 绑定代码(runtime/ui 实现文件)经 impl() 访问 V8 细节(js_runtime_impl.h)。
// 线程约定: momo::Object, 归属 ui 线程, 仅在该线程创建、调用与析构。
//
// 模块: 主程序为 ESM。支持 `momo:*` 原生模块(RegisterNativeModule 注册, synthetic module)
// 与相对/绝对路径的文件模块。Microtask 策略为 explicit: 每次进入 JS 后由调用方 DrainMicrotasks。

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
    // V8 ICU 数据(icudtl.dat)。空或不存在时回退到构建期默认路径;仍失败则 Intl 不可用(记警告)。
    std::filesystem::path icu_data_path;
    // V8 code cache 目录(空 = 不缓存), 见 JsRuntimeImpl::SetCodeCacheDir。
    std::filesystem::path code_cache_dir;
  };

  static StatusOr<std::unique_ptr<JsRuntime>> Create(const Options& options);
  ~JsRuntime() override;

  // 加载并执行 ESM 主模块(含其静态依赖), 随后排空 microtask。
  // 语法错误、解析失败、顶层抛出/拒绝都返回错误(消息含 JS 栈)。
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
