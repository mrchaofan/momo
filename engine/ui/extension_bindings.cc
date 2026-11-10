// ext:* 模块的 V8 胶水(docs/07 §2-4): 把扩展经 C ABI 注册的原生函数暴露为 JS synthetic module,
// 实现 MomoCallApi(参数读取/返回值/抛错/Promise)与 builder_add_function。
// 模块形状(extensions/sdk/types/ext.d.ts): export const native = { fn1, fn2, ... };另以函数名直接导出。

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "engine/extension/extension_host.h"
#include "engine/runtime/js_args.h"
#include "engine/runtime/js_runtime_impl.h"
#include "engine/runtime/js_tags.h"
#include "engine/ui/ui_classes.h"
#include "engine/ui/ui_impl.h"

// C ABI 中的不透明类型(全局命名空间, 与 momo_extension.h 的前置声明一致)。
struct MomoJsBuilder {
  struct Function {
    std::string name;
    MomoNativeFn fn;
    void* user_data;
  };
  std::vector<Function> functions;
};

struct MomoCallInfo {
  const v8::FunctionCallbackInfo<v8::Value>* args;
  momo::engine::ui::UiBindings::Impl* impl;
  bool threw = false;
};

namespace momo::engine::ui {

using runtime::Args;
using runtime::V8String;

namespace {

v8::Isolate* Iso(MomoCallInfoRef c) { return c->args->GetIsolate(); }

size_t ArgCount(MomoCallInfoRef c) { return static_cast<size_t>(c->args->Length()); }

MomoStatus ArgDouble(MomoCallInfoRef c, size_t i, double* out) {
  if (out == nullptr || i >= ArgCount(c)) return kMomoErrInvalidArgs;
  v8::Local<v8::Value> v = (*c->args)[static_cast<int>(i)];
  if (!v->IsNumber()) return kMomoErrInvalidArgs;
  *out = v.As<v8::Number>()->Value();
  return kMomoOk;
}

MomoStatus ArgBool(MomoCallInfoRef c, size_t i, bool* out) {
  if (out == nullptr || i >= ArgCount(c)) return kMomoErrInvalidArgs;
  v8::Local<v8::Value> v = (*c->args)[static_cast<int>(i)];
  if (!v->IsBoolean()) return kMomoErrInvalidArgs;
  *out = v->IsTrue();
  return kMomoOk;
}

// 两段式: buf 为空或太小时只返回 out_len(所需字节数, 不含结尾 0)。
MomoStatus ArgStringUtf8(MomoCallInfoRef c, size_t i, char* buf, size_t buf_len, size_t* out_len) {
  if (out_len == nullptr || i >= ArgCount(c)) return kMomoErrInvalidArgs;
  v8::Local<v8::Value> v = (*c->args)[static_cast<int>(i)];
  if (!v->IsString()) return kMomoErrInvalidArgs;
  const std::string s = runtime::ToUtf8(Iso(c), v);
  *out_len = s.size();
  if (buf != nullptr && buf_len > s.size()) {
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
  } else if (buf != nullptr) {
    return kMomoErrInvalidArgs;  // 缓冲不足(out_len 已给出所需大小)
  }
  return kMomoOk;
}

MomoStatus ArgBytes(MomoCallInfoRef c, size_t i, const uint8_t** out_ptr, size_t* out_len) {
  if (out_ptr == nullptr || out_len == nullptr || i >= ArgCount(c)) return kMomoErrInvalidArgs;
  v8::Local<v8::Value> v = (*c->args)[static_cast<int>(i)];
  if (v->IsArrayBuffer()) {
    auto buffer = v.As<v8::ArrayBuffer>();
    *out_ptr = static_cast<const uint8_t*>(buffer->Data());
    *out_len = buffer->ByteLength();
    return kMomoOk;
  }
  if (v->IsArrayBufferView()) {
    auto view = v.As<v8::ArrayBufferView>();
    *out_ptr = static_cast<const uint8_t*>(view->Buffer()->Data()) + view->ByteOffset();
    *out_len = view->ByteLength();
    return kMomoOk;
  }
  return kMomoErrInvalidArgs;
}

void ReturnDouble(MomoCallInfoRef c, double value) { c->args->GetReturnValue().Set(value); }
void ReturnBool(MomoCallInfoRef c, bool value) { c->args->GetReturnValue().Set(value); }
void ReturnNull(MomoCallInfoRef c) { c->args->GetReturnValue().SetNull(); }

void ReturnString(MomoCallInfoRef c, const char* str, size_t len) {
  if (str == nullptr) {
    ReturnNull(c);
    return;
  }
  c->args->GetReturnValue().Set(V8String(Iso(c), std::string_view(str, len)));
}

void ReturnBytesCopy(MomoCallInfoRef c, const uint8_t* bytes, size_t len) {
  v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(Iso(c), len);
  if (len > 0 && bytes != nullptr) {
    std::memcpy(buffer->Data(), bytes, len);
  }
  c->args->GetReturnValue().Set(buffer);
}

void ReturnPromise(MomoCallInfoRef c, uint64_t* out_token) {
  v8::Local<v8::Promise> promise;
  const uint64_t token = c->impl->NewPromise(&promise);
  c->args->GetReturnValue().Set(promise);
  if (out_token != nullptr) *out_token = token;
}

void ThrowError(MomoCallInfoRef c, const char* message) {
  c->threw = true;
  runtime::ThrowError(Iso(c), message != nullptr ? message : "extension error");
}

const MomoCallApi kCallApi = {
    sizeof(MomoCallApi), &ArgCount,     &ArgDouble,    &ArgBool,      &ArgStringUtf8,
    &ArgBytes,           &ReturnDouble, &ReturnBool,   &ReturnString, &ReturnBytesCopy,
    &ReturnPromise,      &ThrowError,   &ReturnNull,
};

MomoStatus AddFunction(MomoJsBuilderRef builder, const char* name, MomoNativeFn fn,
                       void* user_data) {
  if (builder == nullptr || name == nullptr || *name == '\0' || fn == nullptr) {
    return kMomoErrInvalidArgs;
  }
  for (const auto& f : builder->functions) {
    if (f.name == name) return kMomoErrInvalidArgs;  // 重名
  }
  builder->functions.push_back({name, fn, user_data});
  return kMomoOk;
}

void InvokeExtFunction(const Args& a) {
  auto* binding = static_cast<ExtFunction*>(a.Data().As<v8::External>()->Value(runtime::kExtFnTag));
  MomoCallInfo info{&a, &UiImpl(a.GetIsolate())};
  // 默认返回 undefined;扩展内崩溃无法隔离(同进程), docs/07 §5。
  binding->fn(binding->user_data, &info);
}

}  // namespace

// 安装全部扩展模块(UiBindings::Install 调用, ui 线程)。
void InstallExtensionModules(UiBindings::Impl& impl) {
  ExtensionHost* host = impl.host.extensions;
  if (host == nullptr) {
    return;
  }
  host->SetJsGlue(&kCallApi, &AddFunction);
  host->SetPromiseSink([weak = impl.weak()](uint64_t token, std::optional<std::vector<uint8_t>> bytes,
                                            const std::string& error) {
    UiBindings::Impl* self = weak.get();
    if (self == nullptr) {
      return;
    }
    if (!error.empty()) {
      self->Reject(token, error);
      return;
    }
    self->Settle(token, [&bytes](v8::Isolate* isolate, v8::Local<v8::Context> context,
                                 v8::Local<v8::Promise::Resolver> resolver) {
      v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(isolate, bytes ? bytes->size() : 0);
      if (bytes && !bytes->empty()) {
        std::memcpy(buffer->Data(), bytes->data(), bytes->size());
      }
      resolver->Resolve(context, buffer).Check();
    });
  });
  for (const ExtensionHost::ModuleRegistration& reg : host->modules()) {
    UiBindings::Impl* raw = &impl;
    impl.runtime->RegisterNativeModule(
        reg.specifier, [raw, reg](v8::Isolate* isolate, v8::Local<v8::Context> context,
                                  v8::Local<v8::Object> exports) {
          MomoJsBuilder builder;
          reg.setup(reg.user_data, &builder);  // 扩展在此声明函数(ui 线程)
          v8::Local<v8::Object> native = v8::Object::New(isolate);
          for (const MomoJsBuilder::Function& f : builder.functions) {
            auto binding = std::make_unique<ExtFunction>(
                ExtFunction{f.fn, f.user_data, reg.specifier + "." + f.name});
            v8::Local<v8::External> data =
                v8::External::New(isolate, binding.get(), runtime::kExtFnTag);
            raw->ext_functions.push_back(std::move(binding));
            v8::Local<v8::Function> fn =
                v8::Function::New(context, &InvokeExtFunction, data).ToLocalChecked();
            fn->SetName(V8String(isolate, f.name));
            native->Set(context, V8String(isolate, f.name), fn).Check();
            exports->Set(context, V8String(isolate, f.name), fn).Check();
          }
          exports->Set(context, V8String(isolate, "native"), native).Check();
          MOMO_LOG(VERBOSE) << "Extension module " << reg.specifier << " ("
                            << builder.functions.size() << " functions) from " << reg.extension_id;
        });
  }
}

}  // namespace momo::engine::ui
