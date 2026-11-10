#pragma once

// Wrappable: 由 JS 对象拥有的 C++ 宿主对象(docs/04 §3)。只能被 runtime/ui 实现文件包含。
//  - JS 对象的 internal field 0 指向 Wrappable;类型校验靠 WrapperTypeInfo(无 RTTI)。
//  - 生命周期: JS 包装对象被 GC 时(弱回调)删除 C++ 对象;JsRuntime 销毁时删除仍存活的全部对象。
//  - 线程: 仅 ui 线程。
//
// 子类写法:
//   class Paint final : public Wrappable {
//    public:
//     static const WrapperTypeInfo kTypeInfo;
//     const WrapperTypeInfo* type_info() const override { return &kTypeInfo; }
//   };
//   const WrapperTypeInfo Paint::kTypeInfo = {"Paint"};

#include "base/macros.h"
#include "engine/runtime/js_tags.h"
#include "v8.h"

namespace momo::engine::runtime {

class JsRuntimeImpl;

struct WrapperTypeInfo {
  const char* class_name;
};

inline constexpr int kWrapperInternalFieldCount = 1;

class Wrappable {
 public:
  virtual ~Wrappable();
  virtual const WrapperTypeInfo* type_info() const = 0;

  // 绑定到 JS 对象(须由带 internal field 的模板创建);之后由 JS 对象拥有本对象。
  void Wrap(v8::Isolate* isolate, v8::Local<v8::Object> object);
  v8::Local<v8::Object> GetWrapper(v8::Isolate* isolate) const;

  // 类型安全的解包;value 不是 T 的包装对象时返回 nullptr。
  template <typename T>
  static T* Unwrap(v8::Local<v8::Value> value) {
    Wrappable* w = UnwrapAny(value);
    return (w != nullptr && w->type_info() == &T::kTypeInfo) ? static_cast<T*>(w) : nullptr;
  }
  static Wrappable* UnwrapAny(v8::Local<v8::Value> value);

 protected:
  Wrappable() = default;

 private:
  static void OnWeak(const v8::WeakCallbackInfo<Wrappable>& info);

  v8::Global<v8::Object> wrapper_;
  JsRuntimeImpl* runtime_ = nullptr;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Wrappable);
};

}  // namespace momo::engine::runtime
