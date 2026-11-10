// momo:services: 消息通道的 JS 端(docs/04 §5)。
//   send(channel, data) → Promise<ArrayBuffer|null>   JS → native(MessageChannelHub::SendToNative)
//   setHandler(channel, fn|null)                       native → JS 的处理函数;fn 可返回数据或 Promise
//   isDev / platform

#include <cstring>
#include <utility>

#include "base/logging.h"
#include "engine/runtime/js_args.h"
#include "engine/runtime/js_runtime_impl.h"
#include "engine/ui/ui_classes.h"
#include "engine/ui/ui_impl.h"

namespace momo::engine::ui {

using runtime::Args;
using runtime::ThrowTypeError;
using runtime::ToUtf8;
using runtime::V8String;

namespace {

// ArrayBuffer / ArrayBufferView / null → 字节。非法类型返回 false(已抛错)。
bool BytesOf(v8::Isolate* isolate, v8::Local<v8::Value> value, MessageBytes* out) {
  if (value->IsNullOrUndefined()) {
    out->clear();
    return true;
  }
  if (value->IsArrayBuffer()) {
    v8::Local<v8::ArrayBuffer> buffer = value.As<v8::ArrayBuffer>();
    const auto* data = static_cast<const uint8_t*>(buffer->Data());
    out->assign(data, data + buffer->ByteLength());
    return true;
  }
  if (value->IsArrayBufferView()) {
    v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
    out->resize(view->ByteLength());
    view->CopyContents(out->data(), out->size());
    return true;
  }
  ThrowTypeError(isolate, "message data must be an ArrayBuffer, a typed array or null");
  return false;
}

v8::Local<v8::Value> ToArrayBuffer(v8::Isolate* isolate, const std::optional<MessageBytes>& bytes) {
  if (!bytes) {
    return v8::Null(isolate);
  }
  v8::Local<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(isolate, bytes->size());
  if (!bytes->empty()) {
    std::memcpy(buffer->Data(), bytes->data(), bytes->size());
  }
  return buffer;
}

void Send(const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  UiBindings::Impl& impl = UiImpl(isolate);
  if (a.Length() < 1 || !a[0]->IsString()) {
    ThrowTypeError(isolate, "send(channel, data): channel must be a string");
    return;
  }
  MessageBytes message;
  if (!BytesOf(isolate, a.Length() > 1 ? a[1] : v8::Local<v8::Value>(v8::Null(isolate)),
               &message)) {
    return;
  }
  v8::Local<v8::Promise> promise;
  const uint64_t id = impl.NewPromise(&promise);
  a.GetReturnValue().Set(promise);
  const std::string channel = ToUtf8(isolate, a[0]);
  auto settle = [weak = impl.weak(), id](std::optional<MessageBytes> reply) {
    if (UiBindings::Impl* self = weak.get()) {
      self->Settle(id, [&reply](v8::Isolate* iso, v8::Local<v8::Context> ctx,
                                v8::Local<v8::Promise::Resolver> resolver) {
        resolver->Resolve(ctx, ToArrayBuffer(iso, reply)).Check();
      });
    }
  };
  if (!impl.host.hub) {
    settle(std::nullopt);  // 无消息通道(测试/headless): 回复 null
    return;
  }
  impl.host.hub->SendToNative(channel, std::move(message), impl.task_runner(), std::move(settle));
}

void SetHandler(const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  UiBindings::Impl& impl = UiImpl(isolate);
  if (a.Length() < 1 || !a[0]->IsString()) {
    ThrowTypeError(isolate, "setHandler(channel, handler): channel must be a string");
    return;
  }
  const std::string channel = ToUtf8(isolate, a[0]);
  if (a.Length() < 2 || a[1]->IsNullOrUndefined()) {
    impl.js_handlers.erase(channel);
    return;
  }
  if (!a[1]->IsFunction()) {
    ThrowTypeError(isolate, "setHandler: handler must be a function or null");
    return;
  }
  impl.js_handlers[channel].Reset(isolate, a[1].As<v8::Function>());
}

// handler 返回的 Promise 结算后回复。data 为 pending_replies 的 id。
void OnHandlerFulfilled(const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  UiBindings::Impl& impl = UiImpl(isolate);
  const uint64_t id = static_cast<uint64_t>(a.Data().As<v8::Number>()->Value());
  auto it = impl.pending_replies.find(id);
  if (it == impl.pending_replies.end()) {
    return;
  }
  MessageReply reply = std::move(it->second);
  impl.pending_replies.erase(it);
  MessageBytes bytes;
  v8::TryCatch try_catch(isolate);
  if (a.Length() > 0 && !a[0]->IsNullOrUndefined() && BytesOf(isolate, a[0], &bytes)) {
    reply.Send(std::move(bytes));
  } else {
    reply.Send(std::nullopt);
  }
}

void OnHandlerRejected(const Args& a) {
  v8::Isolate* isolate = a.GetIsolate();
  UiBindings::Impl& impl = UiImpl(isolate);
  const uint64_t id = static_cast<uint64_t>(a.Data().As<v8::Number>()->Value());
  auto it = impl.pending_replies.find(id);
  if (it == impl.pending_replies.end()) {
    return;
  }
  MOMO_LOG(ERROR) << "Message handler rejected: "
                  << (a.Length() > 0 ? ToUtf8(isolate, a[0]) : std::string("?"));
  it->second.Send(std::nullopt);
  impl.pending_replies.erase(it);
}

// native → JS 分发(ui 线程, 由 hub 在端点上调用)。
void DispatchToJs(UiBindings::Impl& impl, const std::string& channel, MessageBytes message,
                  MessageReply reply) {
  auto it = impl.js_handlers.find(channel);
  if (it == impl.js_handlers.end()) {
    reply.Send(std::nullopt);
    return;
  }
  v8::Isolate* isolate = impl.runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = impl.runtime->context();
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Function> handler = it->second.Get(isolate);
  v8::Local<v8::Value> argv[] = {ToArrayBuffer(isolate, std::move(message))};
  auto result = impl.runtime->Call(handler, v8::Undefined(isolate), 1, argv,
                                   "message handler '" + channel + "'");
  if (!result.ok()) {
    reply.Send(std::nullopt);
    impl.runtime->DrainMicrotasks();
    return;
  }
  v8::Local<v8::Value> value = *result;
  if (value->IsPromise()) {
    const uint64_t id = impl.next_reply_++;
    impl.pending_replies.emplace(id, std::move(reply));
    v8::Local<v8::Value> data = v8::Number::New(isolate, static_cast<double>(id));
    v8::Local<v8::Function> on_ok =
        v8::Function::New(context, &OnHandlerFulfilled, data).ToLocalChecked();
    v8::Local<v8::Function> on_err =
        v8::Function::New(context, &OnHandlerRejected, data).ToLocalChecked();
    (void)value.As<v8::Promise>()->Then(context, on_ok, on_err);
  } else {
    MessageBytes bytes;
    v8::TryCatch try_catch(isolate);
    if (!value->IsNullOrUndefined() && BytesOf(isolate, value, &bytes)) {
      reply.Send(std::move(bytes));
    } else {
      reply.Send(std::nullopt);
    }
  }
  impl.runtime->DrainMicrotasks();
}

const char* PlatformName() {
#if defined(__APPLE__)
  return "macos";
#elif defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#else
  return "headless";
#endif
}

}  // namespace

void InstallServicesModule(v8::Isolate* isolate, v8::Local<v8::Context> context,
                           v8::Local<v8::Object> exports) {
  UiBindings::Impl& impl = UiImpl(isolate);
  exports->Set(context, V8String(isolate, "send"),
               v8::Function::New(context, &Send).ToLocalChecked())
      .Check();
  exports->Set(context, V8String(isolate, "setHandler"),
               v8::Function::New(context, &SetHandler).ToLocalChecked())
      .Check();
  exports->Set(context, V8String(isolate, "isDev"),
               v8::Boolean::New(isolate, impl.settings.enable_inspector || impl.settings.dev_mode))
      .Check();
  exports->Set(context, V8String(isolate, "platform"), V8String(isolate, PlatformName())).Check();
  if (impl.host.hub && !impl.services_installed) {
    impl.services_installed = true;
    impl.host.hub->SetJsEndpoint(
        impl.task_runner(), [weak = impl.weak()](const std::string& channel, MessageBytes message,
                                                 MessageReply reply) {
          if (UiBindings::Impl* self = weak.get()) {
            DispatchToJs(*self, channel, std::move(message), std::move(reply));
          } else {
            reply.Send(std::nullopt);
          }
        });
  }
}

}  // namespace momo::engine::ui
