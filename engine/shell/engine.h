#pragma once

// Engine(内部类): ui 线程侧的根(docs/02 §2)。持有各视图的 ViewportMetrics/焦点、帧调度器 Animator
// 与应用驱动 AppDriver。多视图: 一个 Animator(帧时钟)驱动全部视图;视图 0(隐式视图)在创建时即存在,
// 与其他视图一样可被移除。窗口的外观与行为不经过引擎(宿主的 momo/window 通道)。
// 帧路径全在 ui 线程内闭合: driver 请求帧 → Animator 等 vsync → driver.BeginFrame 提交 LayerTree
// → Animator 入队 Pipeline → Delegate::OnEngineDraw(由 Shell 投递 raster)。
// 线程约定: momo::Object, 归属 ui 线程, 仅在 ui 线程创建、调用与析构。

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"
#include "engine/common/settings.h"
#include "engine/platform/input_types.h"
#include "engine/platform/view.h"
#include "engine/platform/viewport_metrics.h"
#include "engine/shell/animator.h"
#include "engine/shell/vsync_waiter.h"
#include "engine/ui/app_driver.h"

namespace momo::engine {

class Engine final : public Object, private ui::WindowClient, private Animator::Delegate {
 public:
  // Engine → Shell, 由 Shell 实现(在 ui 线程被调用)。
  class Delegate {
   public:
    // 有新帧入队: 投递 raster 线程消费。
    virtual void OnEngineDraw(std::shared_ptr<LayerTreePipeline> pipeline) = 0;

   protected:
    ~Delegate() = default;
  };

  // delegate 必须比 Engine 活得久。bundle 加载/执行失败时返回错误(消息含 JS 栈)。
  // host: 驱动可用的引擎设施(消息通道、io runner、扩展、纹理)。
  static StatusOr<std::unique_ptr<Engine>> Create(const Settings& settings, Delegate& delegate,
                                                  std::unique_ptr<VsyncWaiter> waiter,
                                                  const ui::AppHost& host);
  ~Engine() override;

  // 替换 vsync 来源(视图绑定/解绑时)。
  void SetVsyncWaiter(std::unique_ptr<VsyncWaiter> waiter) {
    animator_.SetVsyncWaiter(std::move(waiter));
  }

  // ---- 视图 ----
  // 宿主绑定了一个新视图(视图 0 在创建时即存在)。
  void AddView(platform::ViewId view);
  void RemoveView(platform::ViewId view);
  bool HasView(platform::ViewId view) const { return views_.count(view) != 0; }
  void OnMetricsChanged(platform::ViewId view, const platform::ViewportMetrics& metrics);
  void OnPointerPacket(platform::ViewId view, const platform::PointerDataPacket& packet);
  void OnKeyData(platform::ViewId view, const platform::KeyData& data);
  void OnFocusChanged(platform::ViewId view, bool focused);
  // 视图可见性(窗口隐藏/最小化): 不可见的视图不出帧;全部不可见时停帧。
  void OnVisibilityChanged(platform::ViewId view, bool visible);

  // 主窗口(视图 0)的兼容入口。
  void OnMetricsChanged(const platform::ViewportMetrics& metrics) {
    OnMetricsChanged(platform::kImplicitViewId, metrics);
  }
  void OnPointerPacket(const platform::PointerDataPacket& packet) {
    OnPointerPacket(platform::kImplicitViewId, packet);
  }
  void OnKeyData(const platform::KeyData& data) { OnKeyData(platform::kImplicitViewId, data); }
  void OnFocusChanged(bool focused) { OnFocusChanged(platform::kImplicitViewId, focused); }
  // 应用整体挂起(宿主隐藏了应用)时停帧。应用状态本身属于宿主(momo/app), 引擎只需要知道是否出帧。
  void SetAppSuspended(bool suspended);
  // 宿主(扩展帧回调/纹理新帧)请求出一帧。
  void ScheduleFrameFromHost() { animator_.RequestFrame(); }
  // 由宿主请求重新加载 bundle(--dev hot restart): 销毁 JS 运行时并重新执行 bundle。
  Status HotRestart();

  platform::ViewportMetrics viewport_metrics(platform::ViewId view = platform::kImplicitViewId) const {
    return metrics(view);
  }
  const Animator& animator() const { return animator_; }

 private:
  Engine(const Settings& settings, Delegate& delegate, std::unique_ptr<VsyncWaiter> waiter,
         const ui::AppHost& host);

  // ui::WindowClient
  void ScheduleFrame() override { animator_.RequestFrame(); }
  void Render(std::unique_ptr<graphics::LayerTree> layer_tree) override;
  platform::ViewportMetrics metrics(platform::ViewId view) const override;
  bool focused(platform::ViewId view) const override;
  bool visible(platform::ViewId view) const override;
  std::vector<platform::ViewId> views() const override;

  // Animator::Delegate
  void OnAnimatorBeginFrame(TimePoint frame_start, TimePoint) override;
  void OnAnimatorDraw(std::shared_ptr<LayerTreePipeline> pipeline) override {
    delegate_.OnEngineDraw(std::move(pipeline));
  }

  Delegate& delegate_;
  Settings settings_;
  ui::AppHost host_;
  struct ViewState {
    platform::ViewportMetrics metrics;
    bool focused = false;
    bool visible = true;
  };
  // 帧时钟: 应用未挂起且至少有一个可见视图时运行(零视图不出帧, JS 照常运行)。
  void UpdateAnimator();
  bool app_suspended_ = false;
  std::map<platform::ViewId, ViewState> views_;
  // 先于 driver_ 构造(执行 bundle 时即可能 scheduleFrame), 后于 driver_ 析构。
  Animator animator_;
  std::unique_ptr<ui::AppDriver> driver_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Engine);
};

}  // namespace momo::engine
