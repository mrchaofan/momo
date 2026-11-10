#pragma once

// Engine(内部类): ui 线程侧的根(docs/02 §2)。持有各视图的 ViewportMetrics/焦点、帧调度器 Animator
// 与应用驱动 AppDriver。多视图: 一个 Animator(帧时钟)驱动全部视图;视图 0 为主窗口, 始终存在。
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
#include "engine/shell/lifecycle.h"
#include "engine/shell/pointer_capture.h"
#include "engine/shell/vsync_waiter.h"
#include "engine/ui/app_driver.h"

namespace momo::engine {

class Engine final : public Object, private ui::WindowClient, private Animator::Delegate {
 public:
  // Engine → Shell, 由 Shell 实现(在 ui 线程被调用)。
  // 窗口请求与 Shell 的公开方法同名: Shell 把它们转投 platform 线程, 交给宿主窗口(Shell::Delegate)。
  class Delegate {
   public:
    // 有新帧入队: 投递 raster 线程消费。
    virtual void OnEngineDraw(std::shared_ptr<LayerTreePipeline> pipeline) = 0;
    virtual void SetWindowTitle(platform::ViewId view, std::string title) = 0;
    virtual void SetWindowMinSize(platform::ViewId view, Size logical_size) = 0;
    virtual void SetCursor(platform::ViewId view, platform::SystemCursor cursor) = 0;
    // JS createView: 请求宿主创建窗口;结果经 Engine::OnViewCreateResult 返回。
    virtual void CreateView(uint64_t request, const platform::ViewOptions& options) = 0;
    // JS view.close(): 请求宿主关闭窗口(完成后宿主解绑视图, 经 Engine::RemoveView 通知)。
    virtual void CloseView(platform::ViewId view) = 0;
    virtual void SetWindowAlwaysOnTop(platform::ViewId view, bool on) {
      (void)view;
      (void)on;
    }
    virtual void ActivateWindow(platform::ViewId view) { (void)view; }
    virtual void CaptureWindowPointer(platform::ViewId view) { (void)view; }
    virtual void SetWindowTitleBar(platform::ViewId view, bool hidden, double traffic_center_y) {
      (void)view;
      (void)hidden;
      (void)traffic_center_y;
    }
    virtual void SetWindowDragRegions(platform::ViewId view, std::vector<Rect> rects) {
      (void)view;
      (void)rects;
    }
    virtual void SetWindowPosition(platform::ViewId view, Point top_left) {
      (void)view;
      (void)top_left;
    }

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
  // createView 请求的结果(宿主侧)。
  void OnViewCreateResult(uint64_t request, StatusOr<platform::ViewId> result);

  void OnMetricsChanged(platform::ViewId view, const platform::ViewportMetrics& metrics);
  void OnPointerPacket(platform::ViewId view, const platform::PointerDataPacket& packet);
  void OnKeyData(platform::ViewId view, const platform::KeyData& data);
  void OnFocusChanged(platform::ViewId view, bool focused);
  void OnWindowFrameChanged(platform::ViewId view, const Rect& frame);
  bool OnCloseRequested(platform::ViewId view) { return driver_->OnCloseRequested(view); }

  // 主窗口(视图 0)的兼容入口。
  void OnMetricsChanged(const platform::ViewportMetrics& metrics) {
    OnMetricsChanged(platform::kImplicitViewId, metrics);
  }
  void OnPointerPacket(const platform::PointerDataPacket& packet) {
    OnPointerPacket(platform::kImplicitViewId, packet);
  }
  void OnKeyData(const platform::KeyData& data) { OnKeyData(platform::kImplicitViewId, data); }
  void OnFocusChanged(bool focused) { OnFocusChanged(platform::kImplicitViewId, focused); }
  // suspended/terminating 停帧, resumed 恢复, 然后通知应用。
  void OnLifecycleEvent(LifecycleEvent event);
  bool OnCloseRequested() { return OnCloseRequested(platform::kImplicitViewId); }
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
  void SetTitle(platform::ViewId view, std::string title) override {
    delegate_.SetWindowTitle(view, std::move(title));
  }
  void SetCursor(platform::ViewId view, platform::SystemCursor cursor) override {
    delegate_.SetCursor(view, cursor);
  }
  void SetMinSize(platform::ViewId view, Size logical_size) override {
    delegate_.SetWindowMinSize(view, logical_size);
  }
  platform::ViewportMetrics metrics(platform::ViewId view) const override;
  Rect screen_frame(platform::ViewId view) const override;
  void SetWindowPosition(platform::ViewId view, Point top_left) override {
    delegate_.SetWindowPosition(view, top_left);
  }
  bool focused(platform::ViewId view) const override;
  std::vector<platform::ViewId> views() const override;
  void CreateView(uint64_t request, const platform::ViewOptions& options) override {
    delegate_.CreateView(request, options);
  }
  void CloseView(platform::ViewId view) override { delegate_.CloseView(view); }
  void SetAlwaysOnTop(platform::ViewId view, bool on) override {
    delegate_.SetWindowAlwaysOnTop(view, on);
  }
  void ActivateView(platform::ViewId view) override { delegate_.ActivateWindow(view); }
  bool SetPointerCapture(platform::ViewId view) override;
  void SetTitleBar(platform::ViewId view, bool hidden, double traffic_center_y) override {
    delegate_.SetWindowTitleBar(view, hidden, traffic_center_y);
  }
  void SetDragRegions(platform::ViewId view, std::vector<Rect> rects) override {
    delegate_.SetWindowDragRegions(view, std::move(rects));
  }

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
    Rect frame;
  };
  std::map<platform::ViewId, ViewState> views_;
  // 跨视图指针捕获: 按下所在视图独占该设备的事件直到抬起(拖动经过别的窗口时不被打断)。
  PointerCapture pointer_capture_;
  // 先于 driver_ 构造(执行 bundle 时即可能 scheduleFrame), 后于 driver_ 析构。
  Animator animator_;
  std::unique_ptr<ui::AppDriver> driver_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Engine);
};

}  // namespace momo::engine
