#pragma once

// V8 指针类型标签的统一分配表(只能被 runtime/ui 实现文件包含)。
// V8 用标签区分 External / internal field 中不同 C++ 类型的指针(sandbox/类型混淆防护);
// 取值必须在 V8 允许的范围内(ExternalPointerTypeTag: 0..V8_EXTERNAL_POINTER_TAG_COUNT-2),
// 新增标签只在这里登记, 保证不重复。

#include "v8.h"

namespace momo::engine::runtime {

// v8::External
inline constexpr v8::ExternalPointerTypeTag kCallbackTag = 1;  // ClassBuilder 方法/构造函数指针
inline constexpr v8::ExternalPointerTypeTag kUiImplTag = 2;    // ui::UiBindings::Impl
inline constexpr v8::ExternalPointerTypeTag kExtFnTag = 3;     // ui 扩展函数绑定(ExtFunction)
inline constexpr v8::ExternalPointerTypeTag kUiViewTag = 4;    // ui::ViewBinding(momo:ui 视图对象)

// internal field(Wrappable)
inline constexpr v8::EmbedderDataTypeTag kWrappableTag = 1;

// v8::Isolate::SetData 槽位(0 由 JsRuntimeImpl 占用)。
inline constexpr uint32_t kIsolateSlotUi = 1;  // ui::UiBindings::Impl*

}  // namespace momo::engine::runtime
