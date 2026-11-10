#pragma once

// ClassBuilder<T>: 把 Wrappable 子类 T 暴露为 JS 类(docs/04 §3 NativeClassBuilder)。只能被
// runtime/ui 实现文件包含。方法/访问器以普通函数指针注册, 收到的 this 已做类型校验。
//
//   auto tmpl = ClassBuilder<Paint>(isolate, "Paint", &Paint::Construct)
//                   .Property("color", &Paint::GetColor, &Paint::SetColor)
//                   .Method("copy", &Paint::Copy)
//                   .Export(context, exports);  // 以类名挂到模块导出上(内含 Build)
//   Build() 把类模板登记到 JsRuntimeImpl, 供 NewWrapper<T> 使用。
//
// ctor 为空表示 JS 侧不可 new(只能由原生代码经 NewWrapper 创建, 如 Canvas、Picture)。

#include <memory>
#include <string>

#include "engine/runtime/js_args.h"
#include "engine/runtime/js_runtime_impl.h"
#include "engine/runtime/js_tags.h"
#include "engine/runtime/wrappable.h"
#include "v8.h"

namespace momo::engine::runtime {

template <typename T>
class ClassBuilder {
 public:
  using Ctor = std::unique_ptr<T> (*)(const Args& args);  // 返回空 = 已抛异常
  using MethodFn = void (*)(T& self, const Args& args);

  ClassBuilder(v8::Isolate* isolate, const char* name, Ctor ctor)
      : isolate_(isolate),
        tmpl_(v8::FunctionTemplate::New(
            isolate, &Construct,
            External(reinterpret_cast<void*>(ctor != nullptr ? ctor : &IllegalConstruct)))) {
    tmpl_->SetClassName(V8String(isolate, name));
    tmpl_->InstanceTemplate()->SetInternalFieldCount(kWrapperInternalFieldCount);
    signature_ = v8::Signature::New(isolate, tmpl_);
  }

  ClassBuilder& Method(const char* name, MethodFn fn) {
    tmpl_->PrototypeTemplate()->Set(V8String(isolate_, name), MakeMethod(fn));
    return *this;
  }

  ClassBuilder& Property(const char* name, MethodFn getter, MethodFn setter = nullptr) {
    tmpl_->PrototypeTemplate()->SetAccessorProperty(
        V8String(isolate_, name), MakeMethod(getter),
        setter != nullptr ? MakeMethod(setter) : v8::Local<v8::FunctionTemplate>());
    return *this;
  }

  ClassBuilder& StaticMethod(const char* name, v8::FunctionCallback callback) {
    tmpl_->Set(V8String(isolate_, name), v8::FunctionTemplate::New(isolate_, callback));
    return *this;
  }

  v8::Local<v8::FunctionTemplate> Build() {
    JsRuntimeImpl::From(isolate_)->SetClassTemplate(&T::kTypeInfo, tmpl_);
    return tmpl_;
  }

  // Build 并以类名设为 exports 的属性。
  void Export(v8::Local<v8::Context> context, v8::Local<v8::Object> exports) {
    exports->Set(context, V8String(isolate_, T::kTypeInfo.class_name),
                 Build()->GetFunction(context).ToLocalChecked())
        .Check();
  }

 private:
  v8::Local<v8::External> External(void* p) {
    return v8::External::New(isolate_, p, kCallbackTag);
  }

  v8::Local<v8::FunctionTemplate> MakeMethod(MethodFn fn) {
    return v8::FunctionTemplate::New(isolate_, &Invoke, External(reinterpret_cast<void*>(fn)),
                                     signature_);
  }

  static void Construct(const Args& args) {
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall()) {
      ThrowTypeError(isolate, std::string("Class constructor ") + T::kTypeInfo.class_name +
                                  " cannot be invoked without 'new'");
      return;
    }
    auto ctor = reinterpret_cast<Ctor>(args.Data().As<v8::External>()->Value(kCallbackTag));
    std::unique_ptr<T> object = ctor(args);
    if (object == nullptr) {
      return;  // ctor 已抛出
    }
    object.release()->Wrap(isolate, args.This());
  }

  static std::unique_ptr<T> IllegalConstruct(const Args& args) {
    ThrowTypeError(args.GetIsolate(), std::string("Illegal constructor: ") + T::kTypeInfo.class_name);
    return nullptr;
  }

  static void Invoke(const Args& args) {
    T* self = Wrappable::Unwrap<T>(args.This());
    if (self == nullptr) {
      ThrowTypeError(args.GetIsolate(), std::string("Illegal invocation on ") +
                                            T::kTypeInfo.class_name);
      return;
    }
    auto fn = reinterpret_cast<MethodFn>(args.Data().As<v8::External>()->Value(kCallbackTag));
    fn(*self, args);
  }

  v8::Isolate* isolate_;
  v8::Local<v8::FunctionTemplate> tmpl_;
  v8::Local<v8::Signature> signature_;
};

// 由原生代码创建 T 的 JS 包装对象(不经 JS 构造函数)。T 的类模板须已 Build。
template <typename T>
v8::Local<v8::Object> NewWrapper(v8::Isolate* isolate, std::unique_ptr<T> object) {
  JsRuntimeImpl* runtime = JsRuntimeImpl::From(isolate);
  v8::Local<v8::FunctionTemplate> tmpl = runtime->GetClassTemplate(&T::kTypeInfo);
  v8::Local<v8::Object> wrapper =
      tmpl->InstanceTemplate()->NewInstance(runtime->context()).ToLocalChecked();
  object.release()->Wrap(isolate, wrapper);
  return wrapper;
}

}  // namespace momo::engine::runtime
