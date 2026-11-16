#pragma once

// UiBindings: AppDriver 的 JS 实现(ARCHITECTURE.md §4.7)。拥有 JS 运行时, 向它注册 momo:ui 与
// momo:services, 并把引擎的事件(帧、尺寸、输入、视图增减)交给 JS 注册的回调。头文件不含 V8 类型。
// 应用没有导入 momo:ui 时没有回调可调用: 各入口是空操作, 引擎不出帧。
// 线程约定: 仅 ui 线程。

#include <memory>

#include "base/macros.h"
#include "base/time.h"
#include "engine/ui/app_driver.h"

namespace momo::engine::runtime {
class JsRuntime;
}

namespace momo::engine::ui {

class UiBindings final : public AppDriver {
 public:
  // 接管 runtime 并向它注册 `momo:ui` 与 `momo:services`(须在执行导入它们的模块之前)。client 必须比返回的
  // 对象活得久。host 提供消息通道/io 线程/纹理等设施(测试可传空: 相应功能不可用, 调用时抛错或回复 null)。
  static std::unique_ptr<UiBindings> Install(std::unique_ptr<runtime::JsRuntime> runtime,
                                             WindowClient& client, const AppHost& host = {},
                                             const Settings& settings = {});
  ~UiBindings() override;

  runtime::JsRuntime& runtime() { return *runtime_; }

  // AppDriver: 调用 JS 回调(未设置则忽略)。
  // platformDispatcher.onBeginFrame(frameTimeMs) 然后 onDrawFrame(): 一帧驱动全部视图。
  void BeginFrame(TimePoint frame_start) override;
  // 同步 viewport.devicePixelRatio / physicalSize 后调用 viewport.onMetricsChanged()。
  void OnMetricsChanged(ViewId view) override;
  // viewport.onPointerPacket(data: Float64Array, count)。
  void OnPointerPacket(ViewId view, const platform::PointerDataPacket& packet) override;
  // viewport.onKeyData(data)。
  void OnKeyData(ViewId view, const platform::KeyData& data) override;
  // 同步 viewport.visible 后调用 viewport.onVisibilityChanged(visible)。
  void OnVisibilityChanged(ViewId view, bool visible) override;
  // 视图集合变化: 更新 viewports 后调用 platformDispatcher.onViewportsChanged()。
  void OnViewAdded(ViewId view) override;
  void OnViewRemoved(ViewId view) override;

  // 应用是否导入了 momo:ui。
  bool is_imported() const;
  // 测试/诊断: 已移除但 JS 侧对象尚未被回收的视图绑定数。
  size_t detached_view_count_for_testing() const;

  struct Impl;

 private:
  UiBindings(std::unique_ptr<runtime::JsRuntime> runtime, std::unique_ptr<Impl> impl);

  // 声明顺序即逆序析构: 绑定的状态(impl_)先于运行时销毁。
  std::unique_ptr<runtime::JsRuntime> runtime_;
  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(UiBindings);
};

}  // namespace momo::engine::ui
