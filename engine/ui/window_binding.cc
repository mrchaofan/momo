// momo:ui 的视图(窗口)对象、事件分发与 momo:ui 共享状态(docs/04 §6)。
//
// JS 可见(完整声明见 framework/packages/momo/src/ui/momo-ui.d.ts):
//   View(每个视图一个): id / devicePixelRatio / physicalSize / focused(只读快照, 变化时刷新)
//     render(scene)
//     onMetricsChanged / onPointerPacket(data, count) / onKeyData(data) / onFocusChanged(focused)
//   window: 全局的 scheduleFrame() / onBeginFrame / onDrawFrame / onViewsChanged(不是视图)
//   views: Map<id, View>(隐式视图 0 存在时含它)
//   POINTER_DATA_STRIDE
// 只有引擎职责(画面、尺寸、输入)。窗口的外观与行为(标题、光标、位置、创建/关闭…)经宿主注册的
// momo/window 通道往返, 不经过这里。

#include <chrono>
#include <string_view>
#include <utility>

#include "base/logging.h"
#include "engine/runtime/js_args.h"
#include "engine/runtime/js_runtime.h"
#include "engine/runtime/js_runtime_impl.h"
#include "engine/runtime/js_tags.h"
#include "engine/ui/ui_bindings.h"
#include "engine/ui/ui_classes.h"
#include "engine/ui/ui_impl.h"

namespace momo::engine::ui {

using runtime::Args;
using runtime::ArgNumber;
using runtime::ArgWrapped;
using runtime::JsRuntimeImpl;
using runtime::ToUtf8;
using runtime::V8String;

using runtime::kIsolateSlotUi;
using runtime::kUiImplTag;
using runtime::kUiViewFieldTag;

namespace {

constexpr int kPointerStride = 17;

UiBindings::Impl* ImplOf(const Args& a) {
  return static_cast<UiBindings::Impl*>(a.Data().As<v8::External>()->Value(kUiImplTag));
}

// 视图方法的 data 是视图对象本身(见 AddViewObject), 绑定指针在其 internal field 0。
ViewBinding* ViewOf(const Args& a) {
  return static_cast<ViewBinding*>(
      a.Data().As<v8::Object>()->GetAlignedPointerFromInternalField(0, kUiViewFieldTag));
}

constexpr const char* kViewHandlers[] = {"onMetricsChanged", "onPointerPacket", "onKeyData",
                                         "onFocusChanged"};

void OnDetachedViewWeak(const v8::WeakCallbackInfo<ViewBinding>& info) {
  // 第一遍回调: 只 Reset 句柄并释放 C++ 对象。
  ViewBinding* binding = info.GetParameter();
  binding->object.Reset();
  binding->impl->detached_views.erase(binding);
}

void ScheduleFrame(const Args& a) { ImplOf(a)->client->ScheduleFrame(); }

void Render(const Args& a) {
  ViewBinding* view = ViewOf(a);
  JsScene* scene = ArgWrapped<JsScene>(a, 0);
  if (scene == nullptr || view->removed) {
    return;
  }
  const platform::ViewportMetrics metrics = view->impl->client->metrics(view->id);
  auto tree = std::make_unique<graphics::LayerTree>(metrics.physical_size,
                                                    metrics.device_pixel_ratio, scene->root);
  tree->set_view_id(view->id);
  tree->set_has_platform_views(scene->has_platform_views);
  view->impl->client->Render(std::move(tree));
}

const char* KeyTypeName(platform::KeyEventType type) {
  switch (type) {
    case platform::KeyEventType::kDown: return "down";
    case platform::KeyEventType::kUp: return "up";
    case platform::KeyEventType::kRepeat: return "repeat";
  }
  return "down";
}

}  // namespace

// ---------------------------------------------------------------------------
// UiBindings::Impl

UiBindings::Impl::Impl(JsRuntimeImpl* runtime, WindowClient* client, const AppHost& host,
                       const Settings& settings)
    : runtime(runtime),
      client(client),
      host(host),
      settings(settings),
      assets(settings.assets_dir),
      next_request_(static_cast<uint64_t>(Clock::now().time_since_epoch().count()) << 8 | 1),
      weak_this_(this) {
  runtime->isolate()->SetData(kIsolateSlotUi, this);
}

UiBindings::Impl::~Impl() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  InvalidateWeakPtrs();
  if (host.extensions != nullptr) {
    host.extensions->SetPromiseSink(nullptr);
  }
  if (host.hub && services_installed) {
    host.hub->SetJsEndpoint(nullptr, nullptr);
  }
  runtime->isolate()->SetData(kIsolateSlotUi, nullptr);
}

text::FontCollection& UiBindings::Impl::Fonts() {
  if (fonts == nullptr) {
    fonts = text::FontCollection::Create();
  }
  return *fonts;
}

uint64_t UiBindings::Impl::NewPromise(v8::Local<v8::Promise>* promise) {
  v8::Isolate* isolate = runtime->isolate();
  v8::Local<v8::Promise::Resolver> resolver =
      v8::Promise::Resolver::New(runtime->context()).ToLocalChecked();
  const uint64_t id = next_request_++;
  pending_[id].Reset(isolate, resolver);
  *promise = resolver->GetPromise();
  return id;
}

void UiBindings::Impl::Settle(uint64_t id, const SettleFn& fn) {
  auto it = pending_.find(id);
  if (it == pending_.end()) {
    return;
  }
  v8::Isolate* isolate = runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = runtime->context();
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Promise::Resolver> resolver = it->second.Get(isolate);
  pending_.erase(it);
  fn(isolate, context, resolver);
  runtime->DrainMicrotasks();
}

void UiBindings::Impl::Reject(uint64_t id, const std::string& message) {
  Settle(id, [&message](v8::Isolate* isolate, v8::Local<v8::Context> context,
                        v8::Local<v8::Promise::Resolver> resolver) {
    resolver->Reject(context, v8::Exception::Error(V8String(isolate, message))).Check();
  });
}

UiBindings::Impl& UiImpl(v8::Isolate* isolate) {
  auto* impl = static_cast<UiBindings::Impl*>(isolate->GetData(kIsolateSlotUi));
  MOMO_CHECK(impl != nullptr) << "momo:ui bindings are not installed on this isolate";
  return *impl;
}

text::FontCollection& UiFonts(v8::Isolate* isolate) { return UiImpl(isolate).Fonts(); }

std::shared_ptr<graphics::TextureRegistry> UiTextures(v8::Isolate* isolate) {
  return UiImpl(isolate).host.textures;
}

ViewBinding* UiBindings::Impl::FindView(ViewId id) {
  auto it = views.find(id);
  return it != views.end() ? it->second.get() : nullptr;
}

v8::Local<v8::Object> UiBindings::Impl::AddViewObject(v8::Isolate* isolate,
                                                      v8::Local<v8::Context> context, ViewId id) {
  if (ViewBinding* existing = FindView(id); existing != nullptr && !existing->removed) {
    return existing->object.Get(isolate);
  }
  if (view_template.IsEmpty()) {
    v8::Local<v8::ObjectTemplate> t = v8::ObjectTemplate::New(isolate);
    t->SetInternalFieldCount(1);
    view_template.Reset(isolate, t);
  }
  auto binding = std::make_unique<ViewBinding>();
  binding->impl = this;
  binding->id = id;
  v8::Local<v8::Object> v = view_template.Get(isolate)->NewInstance(context).ToLocalChecked();
  v->SetAlignedPointerInInternalField(0, binding.get(), kUiViewFieldTag);
  // 方法以视图对象为 data: 保存旧方法即保活对象与绑定, 不会悬空。
  v8::Local<v8::Value> data = v;
  auto fn = [&](const char* name, v8::FunctionCallback cb) {
    v->Set(context, V8String(isolate, name),
           v8::Function::New(context, cb, data).ToLocalChecked())
        .Check();
  };
  v->Set(context, V8String(isolate, "id"), v8::Number::New(isolate, static_cast<double>(id)))
      .Check();
  fn("render", &Render);
  for (const char* handler : kViewHandlers) {
    v->Set(context, V8String(isolate, handler), v8::Null(isolate)).Check();
  }
  binding->object.Reset(isolate, v);
  views[id] = std::move(binding);
  if (!views_map.IsEmpty()) {
    views_map.Get(isolate)
        ->Set(context, v8::Number::New(isolate, static_cast<double>(id)), v)
        .ToLocalChecked();
  }
  SyncMetrics(isolate, context, id);
  SyncFocus(isolate, context, id);
  return v;
}

void UiBindings::Impl::DetachView(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                  ViewId id) {
  auto it = views.find(id);
  if (it == views.end()) {
    return;
  }
  std::unique_ptr<ViewBinding> binding = std::move(it->second);
  views.erase(it);
  binding->removed = true;
  if (binding->object.IsEmpty()) {
    return;  // 没有 JS 对象: 直接释放
  }
  // 清空事件回调, 释放 JS 侧闭包(即使 JS 仍持有旧 View 对象)。
  v8::Local<v8::Object> v = binding->object.Get(isolate);
  for (const char* handler : kViewHandlers) {
    v->Set(context, V8String(isolate, handler), v8::Null(isolate)).Check();
  }
  ViewBinding* raw = binding.get();
  raw->object.SetWeak(raw, &OnDetachedViewWeak, v8::WeakCallbackType::kParameter);
  detached_views[raw] = std::move(binding);
}

void UiBindings::Impl::InstallExports(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                      v8::Local<v8::Object> exports) {
  v8::Local<v8::External> data = v8::External::New(isolate, this, kUiImplTag);
  views_map.Reset(isolate, v8::Map::New(isolate));
  // window: 全局的帧调度与帧回调(不是视图;视图都在 views 里, 包括隐式视图 0)。
  v8::Local<v8::Object> w = v8::Object::New(isolate);
  w->Set(context, V8String(isolate, "scheduleFrame"),
         v8::Function::New(context, &ScheduleFrame, data).ToLocalChecked())
      .Check();
  for (const char* handler : {"onBeginFrame", "onDrawFrame", "onViewsChanged"}) {
    w->Set(context, V8String(isolate, handler), v8::Null(isolate)).Check();
  }
  window.Reset(isolate, w);
  // 引擎里已存在的视图(隐式视图 0;热重启后还有其他视图)。
  for (ViewId id : client->views()) {
    AddViewObject(isolate, context, id);
  }
  exports->Set(context, V8String(isolate, "window"), w).Check();
  exports->Set(context, V8String(isolate, "views"), views_map.Get(isolate)).Check();
}

void UiBindings::Impl::SyncMetrics(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                   ViewId id) {
  ViewBinding* view = FindView(id);
  if (view == nullptr || view->object.IsEmpty()) {
    return;
  }
  const platform::ViewportMetrics m = client->metrics(id);
  v8::Local<v8::Object> w = view->object.Get(isolate);
  v8::Local<v8::Object> size = v8::Object::New(isolate);
  size->Set(context, V8String(isolate, "width"), v8::Number::New(isolate, m.physical_size.width))
      .Check();
  size->Set(context, V8String(isolate, "height"), v8::Number::New(isolate, m.physical_size.height))
      .Check();
  w->Set(context, V8String(isolate, "physicalSize"), size).Check();
  w->Set(context, V8String(isolate, "devicePixelRatio"),
         v8::Number::New(isolate, m.device_pixel_ratio))
      .Check();
}

void UiBindings::Impl::SyncFocus(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                 ViewId id) {
  ViewBinding* view = FindView(id);
  if (view != nullptr && !view->object.IsEmpty()) {
    view->object.Get(isolate)
        ->Set(context, V8String(isolate, "focused"), v8::Boolean::New(isolate, client->focused(id)))
        .Check();
  }
}

v8::MaybeLocal<v8::Value> UiBindings::Impl::CallHandler(v8::Local<v8::Object> target,
                                                        const char* name, int argc,
                                                        v8::Local<v8::Value>* argv) {
  v8::Isolate* isolate = runtime->isolate();
  v8::Local<v8::Context> context = runtime->context();
  v8::Local<v8::Value> handler;
  if (!target->Get(context, V8String(isolate, name)).ToLocal(&handler) || !handler->IsFunction()) {
    return {};
  }
  auto result = runtime->Call(handler.As<v8::Function>(), target, argc, argv,
                              std::string("view.") + name);
  if (!result.ok()) {
    return {};
  }
  return *result;
}

v8::MaybeLocal<v8::Value> UiBindings::Impl::CallViewHandler(ViewId id, const char* name, int argc,
                                                            v8::Local<v8::Value>* argv) {
  ViewBinding* view = FindView(id);
  if (view == nullptr || view->removed || view->object.IsEmpty()) {
    return {};
  }
  return CallHandler(view->object.Get(runtime->isolate()), name, argc, argv);
}

// ---------------------------------------------------------------------------
// UiBindings

std::unique_ptr<UiBindings> UiBindings::Install(runtime::JsRuntime& js, WindowClient& client,
                                                const AppHost& host,
                                                const Settings& settings) {
  auto impl = std::make_unique<Impl>(&js.impl(), &client, host, settings);
  Impl* raw = impl.get();
  js.impl().RegisterNativeModule(
      "momo:ui", [raw](v8::Isolate* isolate, v8::Local<v8::Context> context,
                       v8::Local<v8::Object> exports) {
        InstallPaintingClasses(isolate, context, exports);
        InstallSceneClasses(isolate, context, exports);
        InstallTextClasses(isolate, context, exports);
        InstallImageClasses(isolate, context, exports);
        exports->Set(context, V8String(isolate, "POINTER_DATA_STRIDE"),
                     v8::Integer::New(isolate, kPointerStride))
            .Check();
        raw->InstallExports(isolate, context, exports);
      });
  InstallExtensionModules(*raw);
  js.impl().RegisterNativeModule(
      "momo:services", [](v8::Isolate* isolate, v8::Local<v8::Context> context,
                          v8::Local<v8::Object> exports) {
        InstallServicesModule(isolate, context, exports);
      });
  return std::unique_ptr<UiBindings>(new UiBindings(std::move(impl)));
}

UiBindings::UiBindings(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

UiBindings::~UiBindings() = default;

bool UiBindings::is_imported() const { return !impl_->window.IsEmpty(); }

size_t UiBindings::detached_view_count_for_testing() const { return impl_->detached_view_count(); }

void UiBindings::BeginFrame(TimePoint frame_time) {
  if (!is_imported()) {
    return;
  }
  v8::Isolate* isolate = impl_->runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Context::Scope context_scope(impl_->runtime->context());
  const double ms =
      std::chrono::duration<double, std::milli>(frame_time.time_since_epoch()).count();
  v8::Local<v8::Value> argv[] = {v8::Number::New(isolate, ms)};
  impl_->CallHandler(impl_->window.Get(isolate), "onBeginFrame", 1, argv);
  impl_->runtime->DrainMicrotasks();
}

void UiBindings::DrawFrame() {
  if (!is_imported()) {
    return;
  }
  v8::Isolate* isolate = impl_->runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Context::Scope context_scope(impl_->runtime->context());
  impl_->CallHandler(impl_->window.Get(isolate), "onDrawFrame", 0, nullptr);
  impl_->runtime->DrainMicrotasks();
}

void UiBindings::MetricsChanged(ViewId view) {
  if (!is_imported()) {
    return;
  }
  v8::Isolate* isolate = impl_->runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = impl_->runtime->context();
  v8::Context::Scope context_scope(context);
  impl_->SyncMetrics(isolate, context, view);
  impl_->CallViewHandler(view, "onMetricsChanged", 0, nullptr);
  impl_->runtime->DrainMicrotasks();
}

void UiBindings::PointerPacket(ViewId view, const platform::PointerDataPacket& packet) {
  if (!is_imported() || packet.empty()) {
    return;
  }
  v8::Isolate* isolate = impl_->runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Context::Scope context_scope(impl_->runtime->context());
  const size_t needed = packet.size() * kPointerStride;
  // 复用缓冲(docs/04 §6: 事件对象使用预分配的 TypedArray 视图), 容量不足时按 2 倍增长。
  if (impl_->pointer_buffer.IsEmpty() || impl_->pointer_capacity < needed) {
    impl_->pointer_capacity = std::max<size_t>(needed, impl_->pointer_capacity * 2);
    v8::Local<v8::ArrayBuffer> buffer =
        v8::ArrayBuffer::New(isolate, impl_->pointer_capacity * sizeof(double));
    impl_->pointer_buffer.Reset(
        isolate, v8::Float64Array::New(buffer, 0, impl_->pointer_capacity));
  }
  v8::Local<v8::Float64Array> data = impl_->pointer_buffer.Get(isolate);
  auto* out = static_cast<double*>(data->Buffer()->Data());
  for (const platform::PointerData& p : packet) {
    out[0] = static_cast<double>(p.timestamp_us);
    out[1] = static_cast<double>(p.kind);
    out[2] = static_cast<double>(p.phase);
    out[3] = p.device;
    out[4] = p.physical_x;
    out[5] = p.physical_y;
    out[6] = p.buttons;
    out[7] = p.scroll_delta_x;
    out[8] = p.scroll_delta_y;
    out[9] = p.momentum ? 1 : 0;
    out[10] = p.screen_x;
    out[11] = p.screen_y;
    out[12] = static_cast<double>(p.sequence_id);
    out[13] = static_cast<double>(p.signal_phase);
    out[14] = static_cast<double>(p.gesture_kind);
    out[15] = p.gesture_value;
    out[16] = p.gesture_stage;
    out += kPointerStride;
  }
  v8::Local<v8::Value> argv[] = {data, v8::Integer::New(isolate, static_cast<int>(packet.size()))};
  impl_->CallViewHandler(view, "onPointerPacket", 2, argv);
  impl_->runtime->DrainMicrotasks();
}

bool UiBindings::KeyData(ViewId view, const platform::KeyData& data) {
  if (!is_imported()) {
    return false;
  }
  v8::Isolate* isolate = impl_->runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = impl_->runtime->context();
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Object> o = v8::Object::New(isolate);
  auto set = [&](const char* k, v8::Local<v8::Value> v) {
    o->Set(context, V8String(isolate, k), v).Check();
  };
  set("type", V8String(isolate, KeyTypeName(data.type)));
  set("timeStampUs", v8::Number::New(isolate, static_cast<double>(data.timestamp_us)));
  set("physicalKey", v8::Number::New(isolate, static_cast<double>(data.physical_key)));
  set("logicalKey", v8::Number::New(isolate, static_cast<double>(data.logical_key)));
  set("character", V8String(isolate, data.character));
  set("modifiers", v8::Number::New(isolate, data.modifiers));
  v8::Local<v8::Value> argv[] = {o};
  v8::Local<v8::Value> result;
  const bool handled =
      impl_->CallViewHandler(view, "onKeyData", 1, argv).ToLocal(&result) && result->IsTrue();
  impl_->runtime->DrainMicrotasks();
  return handled;
}

void UiBindings::FocusChanged(ViewId view, bool focused) {
  if (!is_imported()) {
    return;
  }
  v8::Isolate* isolate = impl_->runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = impl_->runtime->context();
  v8::Context::Scope context_scope(context);
  impl_->SyncFocus(isolate, context, view);
  v8::Local<v8::Value> argv[] = {v8::Boolean::New(isolate, focused)};
  impl_->CallViewHandler(view, "onFocusChanged", 1, argv);
  impl_->runtime->DrainMicrotasks();
}

void UiBindings::ViewAdded(ViewId view) {
  if (!is_imported()) {
    return;
  }
  v8::Isolate* isolate = impl_->runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = impl_->runtime->context();
  v8::Context::Scope context_scope(context);
  impl_->AddViewObject(isolate, context, view);
  impl_->CallHandler(impl_->window.Get(isolate), "onViewsChanged", 0, nullptr);
  impl_->runtime->DrainMicrotasks();
}

void UiBindings::ViewRemoved(ViewId view) {
  ViewBinding* binding = impl_->FindView(view);
  if (!is_imported() || binding == nullptr || binding->removed) {
    return;
  }
  v8::Isolate* isolate = impl_->runtime->isolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context = impl_->runtime->context();
  v8::Context::Scope context_scope(context);
  // JS 可能仍持有对象或其方法: 之后的 render/title 等调用为空操作;引擎只弱持有, 不可达时回收。
  impl_->views_map.Get(isolate)->Delete(context, v8::Number::New(isolate, static_cast<double>(view)))
      .Check();
  impl_->DetachView(isolate, context, view);
  impl_->CallHandler(impl_->window.Get(isolate), "onViewsChanged", 0, nullptr);
  impl_->runtime->DrainMicrotasks();
}

}  // namespace momo::engine::ui
