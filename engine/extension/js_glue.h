#pragma once

// ExtensionJsGlue: MomoHostApi 中依赖 JS 引擎的部分——调用参数访问表(MomoCallApi)与 builder_add_function。
// 由 ui 模块在链接期提供(ui::GetExtensionJsGlue, engine/ui/app_driver.h): 有 V8 时是
// engine/ui/extension_bindings.cc 的实现, 无 V8 的构建是 engine/ui/app_driver_no_js.cc 里返回错误的实现。
// ExtensionHost 构造时把它填进函数表, 此后函数表不再改动(扩展可以放心拷贝 MomoHostApi)。

#include "momo/momo_extension.h"

namespace momo::engine {

struct ExtensionJsGlue {
  const MomoCallApi* call_api = nullptr;
  MomoStatus (*add_function)(MomoJsBuilderRef, const char* name, MomoNativeFn fn,
                             void* user_data) = nullptr;
};

}  // namespace momo::engine
