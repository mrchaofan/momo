#pragma once

// Engine(内部类): ui 线程侧的根(docs/02 §2)。持有各视图的 ViewportMetrics/焦点、帧调度器 Animator
// 与应用驱动 AppDriver。多视图: 一个 Animator(帧时钟)驱动全部视图;视图全部来自宿主的绑定(AddView),
// 可以先于 JS 运行时存在。窗口的外观与行为不经过引擎(宿主的 momo/window 通道)。
// JS 运行时的生命周期与 Engine 分开: 构造出来的 Engine 没有运行时(不出帧), StartRuntime 创建驱动并执行
// bundle, StopRuntime 销毁它;首次启动与 hot restart 走的是同一对入口。
// 帧路径全在 ui 线程内闭合: driver 请求帧 → Animator 等 vsync → driver.BeginFrame 提交 LayerTree
// → Animator 入队 Pipeline → 构造时传入的 on_draw(由 Shell 投递 raster)。
// 线程约定: momo::Object, 归属 ui 线程, 仅在 ui 线程创建、调用与析构。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
  // 有新帧入队(ui 线程调用): Shell 据此投递 raster 线程消费。
  using DrawCallback = std::function<void(std::shared_ptr<LayerTreePipeline>)>;

  // host: 驱动可用的引擎设施(消息通道、io runner、扩展、纹理);其中的指针成员须比 Engine 活得久。
  // 构造时成为纹理新帧与扩展帧回调的帧请求端(都在 ui 线程), 析构时注销。
  Engine(const Settings& settings, DrawCallback on_draw, std::unique_ptr<VsyncWaiter> waiter,
         const ui::AppHost& host);
  ~Engine() override;

  // ---- JS 运行时 ----
  // 创建第 generation 代运行时并执行 bundle(同步;调用前不得有运行时), 然后把已有视图的状态(尺寸、焦点、
  // 可见性)交给它并请求一帧。bundle 加载/执行失败时退回占位驱动(引擎保持可用, 修复后可再次启动)并返回
  // 错误(消息含 JS 栈)。
  Status StartRuntime(uint64_t generation);
  // 销毁运行时(幂等)。之后不出帧、不执行扩展帧回调, 直到下一次 StartRuntime;视图状态保留。
  void StopRuntime();

  // 替换 vsync 来源(视图绑定/解绑时)。
  void SetVsyncWaiter(std::unique_ptr<VsyncWaiter> waiter) {
    animator_.SetVsyncWaiter(std::move(waiter));
  }

  // ---- 视图 ----
  void AddView(platform::ViewId view);
  void RemoveView(platform::ViewId view);
  void OnMetricsChanged(platform::ViewId view, const platform::ViewportMetrics& metrics);
  void OnPointerPacket(platform::ViewId view, const platform::PointerDataPacket& packet);
  void OnKeyData(platform::ViewId view, const platform::KeyData& data);
  void OnFocusChanged(platform::ViewId view, bool focused);
  // 视图可见性(窗口隐藏/最小化): 不可见的视图不出帧;全部不可见时停帧。
  void OnVisibilityChanged(platform::ViewId view, bool visible);

  // 应用整体挂起(宿主隐藏了应用)时停帧。应用状态本身属于宿主(momo/app), 引擎只需要知道是否出帧。
  void SetAppSuspended(bool suspended);

 private:
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
    on_draw_(std::move(pipeline));
  }

  const DrawCallback on_draw_;
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
  // 运行时(JS 驱动或占位驱动);StartRuntime 之前与 StopRuntime 之后为空。
  std::unique_ptr<ui::AppDriver> driver_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Engine);
};

}  // namespace momo::engine
