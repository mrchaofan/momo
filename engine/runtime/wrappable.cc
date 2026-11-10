#include "engine/runtime/wrappable.h"

#include "base/logging.h"
#include "engine/runtime/js_runtime_impl.h"

namespace momo::engine::runtime {

Wrappable::~Wrappable() {
  if (runtime_ != nullptr) {
    runtime_->UntrackWrappable(this);
  }
  wrapper_.Reset();
}

void Wrappable::Wrap(v8::Isolate* isolate, v8::Local<v8::Object> object) {
  MOMO_DCHECK(wrapper_.IsEmpty()) << "Wrappable wrapped twice";
  MOMO_CHECK(object->InternalFieldCount() >= kWrapperInternalFieldCount);
  object->SetAlignedPointerInInternalField(0, this, kWrappableTag);
  wrapper_.Reset(isolate, object);
  wrapper_.SetWeak(this, &Wrappable::OnWeak, v8::WeakCallbackType::kParameter);
  runtime_ = JsRuntimeImpl::From(isolate);
  runtime_->TrackWrappable(this);
}

v8::Local<v8::Object> Wrappable::GetWrapper(v8::Isolate* isolate) const {
  return wrapper_.Get(isolate);
}

Wrappable* Wrappable::UnwrapAny(v8::Local<v8::Value> value) {
  if (value.IsEmpty() || !value->IsObject()) {
    return nullptr;
  }
  v8::Local<v8::Object> object = value.As<v8::Object>();
  if (object->InternalFieldCount() < kWrapperInternalFieldCount) {
    return nullptr;
  }
  return static_cast<Wrappable*>(object->GetAlignedPointerFromInternalField(0, kWrappableTag));
}

void Wrappable::OnWeak(const v8::WeakCallbackInfo<Wrappable>& info) {
  // 第一遍回调: 只允许 Reset 句柄与释放 C++ 资源(析构不调用其他 V8 API)。
  Wrappable* self = info.GetParameter();
  self->wrapper_.Reset();
  delete self;
}

}  // namespace momo::engine::runtime
