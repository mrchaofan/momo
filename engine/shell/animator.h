#pragma once

// Animator: ui 线程的帧调度器(docs/05 §5)。
//   RequestFrame() → VsyncWaiter → BeginFrame: 预占 Pipeline 槽位 → Delegate::OnAnimatorBeginFrame
//   (Engine 驱动 JS onBeginFrame/onDrawFrame)→ 期间各视图 Render(layer_tree) 收集进本帧 FrameItem
//   → 帧结束时整体入队 → OnAnimatorDraw。
// 多视图: 一个帧时钟驱动全部视图;一帧里每个视图(LayerTree::view_id)最多提交一棵树, 同帧重复提交被丢弃。
// 背压: Pipeline 满(raster 落后)时跳过本次 vsync 并继续等待下一次, 不驱动 JS(丢 build 不丢最新画面)。
// 空闲零功耗: 无 RequestFrame 时不请求 vsync。Stop()(Suspended)期间的请求在 Start() 时补发。
// 线程约定: momo::Object, 归属 ui 线程, 仅在 ui 线程创建、调用与析构。

#include <cstdint>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/object.h"
#include "base/pipeline.h"
#include "base/task_runner.h"
#include "base/time.h"
#include "engine/graphics/layer_tree.h"
#include "engine/shell/vsync_waiter.h"

namespace momo::engine {

// 一帧的产物: 本帧提交了画面的各视图的层树(每个视图至多一棵)。
struct FrameItem {
  std::vector<std::unique_ptr<graphics::LayerTree>> layer_trees;
};

using LayerTreePipeline = Pipeline<std::unique_ptr<FrameItem>>;

class Animator final : public Object {
 public:
  // 回调均在 ui 线程。
  class Delegate {
   public:
    // 一帧开始: 期间可调用 Animator::Render 提交本帧。
    virtual void OnAnimatorBeginFrame(TimePoint frame_start, TimePoint frame_target) = 0;
    // 有新帧(FrameItem)入队: 由 Shell 投递 raster 线程消费。
    virtual void OnAnimatorDraw(std::shared_ptr<LayerTreePipeline> pipeline) = 0;

   protected:
    ~Delegate() = default;
  };

  struct Stats {
    uint64_t frames_begun = 0;      // 驱动了 OnAnimatorBeginFrame 的 vsync
    uint64_t frames_rendered = 0;   // 提交进 Pipeline 的帧(含至少一棵层树)
    uint64_t frames_skipped = 0;    // 因 Pipeline 满而跳过的 vsync
    uint64_t renders_dropped = 0;   // 帧外(或同帧重复)Render 被丢弃
  };

  static constexpr size_t kPipelineDepth = 2;  // 排队 1 + 正在绘制 1

  Animator(Delegate& delegate, std::shared_ptr<TaskRunner> ui_runner,
           std::unique_ptr<VsyncWaiter> waiter);
  ~Animator() override;

  // 替换 vsync 来源(视图绑定/解绑时)。正在等待的请求转到新 waiter。
  void SetVsyncWaiter(std::unique_ptr<VsyncWaiter> waiter);

  // 请求一帧。重复请求合并。
  void RequestFrame();

  // 在 OnAnimatorBeginFrame 内调用, 提交本帧中 layer_tree->view_id() 视图的画面。
  // 帧外调用、或同一视图在同帧第二次调用会被丢弃。
  void Render(std::unique_ptr<graphics::LayerTree> layer_tree);

  // 生命周期: Suspended 时 Stop, Resumed 时 Start。默认运行。
  void Start();
  void Stop();
  bool is_running() const { return running_; }

  bool frame_scheduled() const { return frame_scheduled_; }
  const Stats& stats() const { return stats_; }
  const std::shared_ptr<LayerTreePipeline>& pipeline() const { return pipeline_; }

 private:
  void AwaitVsync();
  void BeginFrame(TimePoint frame_start, TimePoint frame_target);

  Delegate& delegate_;
  std::unique_ptr<VsyncWaiter> waiter_;
  std::shared_ptr<LayerTreePipeline> pipeline_;

  bool running_ = true;
  bool frame_scheduled_ = false;   // 已向 waiter 请求 vsync
  bool pending_on_start_ = false;  // Stop 期间有帧请求
  // 当前帧预占的槽位与收集中的产物;仅在 BeginFrame 期间有效。
  LayerTreePipeline::ProducerContinuation continuation_;
  std::unique_ptr<FrameItem> current_;
  TimePoint frame_start_;
  TimePoint frame_target_;
  TimePoint build_start_;
  Stats stats_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Animator);
};

}  // namespace momo::engine
