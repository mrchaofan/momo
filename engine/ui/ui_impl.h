#pragma once

// UiBindings::Impl: 一个 JS 运行时上 momo:ui / momo:services 绑定的共享状态。只被 engine/ui 实现文件包含。
// 线程约定: momo::Object, 归属 ui 线程。异步操作(图片解码、资源读取、消息通道)的结果经
// WeakPtr<Impl> 投递回 ui 线程: 运行时已销毁(hot restart/关闭)时回调作废。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "base/object.h"
#include "engine/assets/asset_manager.h"
#include "engine/extension/extension_host.h"
#include "engine/runtime/js_runtime_impl.h"
#include "engine/text/font_collection.h"
#include "engine/ui/app_driver.h"
#include "engine/ui/ui_bindings.h"
#include "v8.h"

namespace momo::engine::ui {

// 一个已绑定的扩展函数(ext:* 模块, 见 extension_bindings.cc);随 Impl 存活, V8 函数的 data 指向它。
struct ExtFunction {
  MomoNativeFn fn;
  void* user_data;
  std::string qualified_name;  // "ext:hello.add"
};

// 一个视图(窗口)在 JS 中的对象。对象 internal field 0 指向本绑定, 对象上的方法以该对象为
// callback data(方法 → 对象 → 绑定), 因此只要 JS 还持有对象或其任一方法, 绑定就有效。
// 生命周期: 视图存在时由 Impl::views 持有, object 为强句柄;视图移除后标记 removed、清空事件回调,
// 移入 Impl::detached_views 并把 object 改为弱句柄, JS 不再可达时由弱回调释放。
struct ViewBinding {
  UiBindings::Impl* impl;
  ViewId id;
  bool removed = false;
  v8::Global<v8::Object> object;
};

struct UiBindings::Impl final : public Object {
  Impl(runtime::JsRuntimeImpl* runtime, WindowClient* client, const AppHost& host,
       const Settings& settings);
  ~Impl() override;

  runtime::JsRuntimeImpl* const runtime;
  WindowClient* const client;
  const AppHost host;
  const Settings settings;
  // 资源读取(io 线程使用;无可变状态, 以 shared_ptr 随任务传递)。
  const assets::AssetManager assets;

  v8::Global<v8::Object> window;  // momo:ui 被导入后才存在(全局帧调度与帧回调;不是视图)
  v8::Global<v8::Map> views_map;  // 导出的 views: Map<id, View>
  std::map<ViewId, std::unique_ptr<ViewBinding>> views;
  // 已移除、等待 JS 侧对象被回收的视图绑定(object 为弱句柄)。
  std::map<ViewBinding*, std::unique_ptr<ViewBinding>> detached_views;
  v8::Global<v8::ObjectTemplate> view_template;  // 带 1 个 internal field 的视图对象模板
  v8::Global<v8::Float64Array> pointer_buffer;
  size_t pointer_capacity = 0;

  // momo:services
  bool services_installed = false;
  std::map<std::string, v8::Global<v8::Function>> js_handlers;
  uint64_t next_reply_ = 1;
  std::map<uint64_t, MessageReply> pending_replies;  // JS handler 返回 Promise 时暂存
  std::vector<std::unique_ptr<ExtFunction>> ext_functions;

  text::FontCollection& Fonts();

  // ---- Promise 支持 ----
  using SettleFn = std::function<void(v8::Isolate*, v8::Local<v8::Context>,
                                      v8::Local<v8::Promise::Resolver>)>;
  // 创建 pending promise, 返回其 id。
  uint64_t NewPromise(v8::Local<v8::Promise>* promise);
  // 结算(在 ui 线程, 自建 HandleScope/Context, 之后排空 microtask)。id 不存在时忽略。
  void Settle(uint64_t id, const SettleFn& fn);
  void Reject(uint64_t id, const std::string& message);
  const WeakPtr<Impl>& weak() const { return weak_this_; }

  // 安装导出: window / views。
  void InstallExports(v8::Isolate* isolate, v8::Local<v8::Context> context,
                      v8::Local<v8::Object> exports);
  // 创建视图对象并登记进 views(已存在则返回已有的)。
  v8::Local<v8::Object> AddViewObject(v8::Isolate* isolate, v8::Local<v8::Context> context,
                                      ViewId id);
  ViewBinding* FindView(ViewId id);
  // 视图已移除: 清空事件回调, 转为弱持有(JS 不再引用时释放)。
  void DetachView(v8::Isolate* isolate, v8::Local<v8::Context> context, ViewId id);
  // 测试/诊断: 仍存活的已移除视图绑定数。
  size_t detached_view_count() const { return detached_views.size(); }
  void SyncMetrics(v8::Isolate* isolate, v8::Local<v8::Context> context, ViewId id);
  void SyncFocus(v8::Isolate* isolate, v8::Local<v8::Context> context, ViewId id);
  void SyncVisible(v8::Isolate* isolate, v8::Local<v8::Context> context, ViewId id);
  // 调用 target[name](args...);未设置或非函数时返回空。
  v8::MaybeLocal<v8::Value> CallHandler(v8::Local<v8::Object> target, const char* name,
                                        int argc, v8::Local<v8::Value>* argv);
  // 视图对象上的处理器(视图不存在时返回空)。
  v8::MaybeLocal<v8::Value> CallViewHandler(ViewId id, const char* name, int argc,
                                            v8::Local<v8::Value>* argv);

 private:
  std::unique_ptr<text::FontCollection> fonts;
  // Promise id 也是扩展 promise token(跨线程往返)。起点取自单调时钟, 使 hot restart 前后两个
  // 运行时的 id 不重叠, 旧运行时的迟到结算不会命中新运行时的 promise。
  uint64_t next_request_;
  std::map<uint64_t, v8::Global<v8::Promise::Resolver>> pending_;
  const WeakPtr<Impl> weak_this_;
};

}  // namespace momo::engine::ui
