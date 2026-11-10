#pragma once

// 绑定函数的参数解码/返回值编码小工具。只能被 runtime/ui 实现文件包含。
// 约定: 解码失败时已向 JS 抛出 TypeError 并返回 false/nullptr, 调用方直接 return。

#include <cstdint>
#include <string>
#include <string_view>

#include "engine/runtime/wrappable.h"
#include "v8.h"

namespace momo::engine::runtime {

using Args = v8::FunctionCallbackInfo<v8::Value>;

v8::Local<v8::String> V8String(v8::Isolate* isolate, std::string_view utf8);
std::string ToUtf8(v8::Isolate* isolate, v8::Local<v8::Value> value);
void ThrowTypeError(v8::Isolate* isolate, std::string_view message);
void ThrowError(v8::Isolate* isolate, std::string_view message);

// 第 index 个参数为有限数字。
bool ArgNumber(const Args& args, int index, double* out);
// 第 index 个参数为数字, 截断为 uint32(颜色等)。
bool ArgUint32(const Args& args, int index, uint32_t* out);
// 可选数字参数: 缺省/undefined 时取 fallback。
bool OptNumber(const Args& args, int index, double fallback, double* out);
bool ArgFunction(const Args& args, int index, v8::Local<v8::Function>* out);

// 第 index 个参数为 T 的包装对象。
template <typename T>
T* ArgWrapped(const Args& args, int index) {
  T* object = index < args.Length() ? Wrappable::Unwrap<T>(args[index]) : nullptr;
  if (object == nullptr) {
    ThrowTypeError(args.GetIsolate(), std::string("argument ") + std::to_string(index + 1) +
                                          " must be a " + T::kTypeInfo.class_name);
  }
  return object;
}

// 把 value 设为当前调用的返回值。
inline void Return(const Args& args, double value) { args.GetReturnValue().Set(value); }

}  // namespace momo::engine::runtime
