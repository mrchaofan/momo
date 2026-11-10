#include "engine/runtime/js_args.h"

#include <cmath>

namespace momo::engine::runtime {

v8::Local<v8::String> V8String(v8::Isolate* isolate, std::string_view utf8) {
  return v8::String::NewFromUtf8(isolate, utf8.data(), v8::NewStringType::kNormal,
                                 static_cast<int>(utf8.size()))
      .ToLocalChecked();
}

std::string ToUtf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value utf8(isolate, value);
  return *utf8 != nullptr ? std::string(*utf8, utf8.length()) : std::string();
}

void ThrowTypeError(v8::Isolate* isolate, std::string_view message) {
  isolate->ThrowException(v8::Exception::TypeError(V8String(isolate, message)));
}

void ThrowError(v8::Isolate* isolate, std::string_view message) {
  isolate->ThrowException(v8::Exception::Error(V8String(isolate, message)));
}

bool ArgNumber(const Args& args, int index, double* out) {
  if (index >= args.Length() || !args[index]->IsNumber()) {
    ThrowTypeError(args.GetIsolate(),
                   "argument " + std::to_string(index + 1) + " must be a number");
    return false;
  }
  *out = args[index].As<v8::Number>()->Value();
  if (!std::isfinite(*out)) {
    ThrowTypeError(args.GetIsolate(),
                   "argument " + std::to_string(index + 1) + " must be a finite number");
    return false;
  }
  return true;
}

bool ArgUint32(const Args& args, int index, uint32_t* out) {
  double value = 0;
  if (!ArgNumber(args, index, &value)) {
    return false;
  }
  *out = static_cast<uint32_t>(static_cast<int64_t>(value));
  return true;
}

bool OptNumber(const Args& args, int index, double fallback, double* out) {
  if (index >= args.Length() || args[index]->IsUndefined()) {
    *out = fallback;
    return true;
  }
  return ArgNumber(args, index, out);
}

bool ArgFunction(const Args& args, int index, v8::Local<v8::Function>* out) {
  if (index >= args.Length() || !args[index]->IsFunction()) {
    ThrowTypeError(args.GetIsolate(),
                   "argument " + std::to_string(index + 1) + " must be a function");
    return false;
  }
  *out = args[index].As<v8::Function>();
  return true;
}

}  // namespace momo::engine::runtime
