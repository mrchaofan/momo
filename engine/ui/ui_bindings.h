#pragma once

// momo:ui 绑定的引擎侧接口(docs/02 §4、docs/04 §6)。不含 V8 类型: JsAppDriver 经它安装模块并分发事件。
// 线程约定: 仅 ui 线程。

#include <memory>
#include <string>

#include "base/macros.h"
#include "base/time.h"
#include "engine/ui/app_driver.h"  // WindowClient

namespace momo::engine::runtime {
class JsRuntime;
}

namespace momo::engine::ui {

class UiBindings {
 public:
  // 向 runtime 注册 `momo:ui` 与 `momo:services` 原生模块。runtime 与 client 必须比返回的对象活得久。
  // host 提供消息通道/io 线程/纹理等设施(测试可传空: 相应功能不可用, 调用时抛错或回复 null)。
  static std::unique_ptr<UiBindings> Install(runtime::JsRuntime& runtime, WindowClient& client,
                                             const AppHost& host = {},
                                             const Settings& settings = {});
  ~UiBindings();

  // ---- 事件入口: 调用 JS 回调(未设置则忽略), 之后排空 microtask ----
  // 帧回调挂在全局的 window 对象上, 一帧驱动全部视图。
  // frame_time: vsync 时间, 以毫秒传给 window.onBeginFrame(frameTimeMs)。
  void BeginFrame(TimePoint frame_time);
  void DrawFrame();
  // 以下按视图: 调用 views.get(view) 上的回调。
  // 同步 view.devicePixelRatio / physicalSize 后调用 view.onMetricsChanged()。
  void MetricsChanged(ViewId view);
  // view.onPointerPacket(data: Float64Array, count)。
  void PointerPacket(ViewId view, const platform::PointerDataPacket& packet);
  // view.onKeyData(data) 返回 true 表示已消费。
  bool KeyData(ViewId view, const platform::KeyData& data);
  void FocusChanged(ViewId view, bool focused);
  // 同步 viewport.visible 后调用 viewport.onVisibilityChanged(visible)。
  void VisibilityChanged(ViewId view, bool visible);
  // 视图集合变化: 更新 viewports 后调用 platformDispatcher.onViewportsChanged()。
  void ViewAdded(ViewId view);
  void ViewRemoved(ViewId view);

  // 隐式视图(视图 0)的便捷入口。
  void MetricsChanged() { MetricsChanged(platform::kImplicitViewId); }
  void PointerPacket(const platform::PointerDataPacket& packet) {
    PointerPacket(platform::kImplicitViewId, packet);
  }
  bool KeyData(const platform::KeyData& data) { return KeyData(platform::kImplicitViewId, data); }
  void FocusChanged(bool focused) { FocusChanged(platform::kImplicitViewId, focused); }

  // 应用是否导入了 momo:ui(未导入时上述回调都是空操作)。
  bool is_imported() const;
  // 测试/诊断: 已移除但 JS 侧对象尚未被回收的视图绑定数。
  size_t detached_view_count_for_testing() const;

  struct Impl;

 private:
  explicit UiBindings(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(UiBindings);
};

}  // namespace momo::engine::ui
