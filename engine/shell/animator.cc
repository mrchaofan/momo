#include "engine/shell/animator.h"

#include <utility>

#include "base/logging.h"

namespace momo::engine {

Animator::Animator(Delegate& delegate, std::shared_ptr<TaskRunner> ui_runner,
                   std::unique_ptr<VsyncWaiter> waiter)
    : Object(std::move(ui_runner)),
      delegate_(delegate),
      waiter_(std::move(waiter)),
      pipeline_(LayerTreePipeline::Create(kPipelineDepth)) {
  MOMO_CHECK(waiter_ != nullptr);
  MOMO_DCHECK_ON_AFFINITY_THREAD();
}

Animator::~Animator() { MOMO_DCHECK_ON_AFFINITY_THREAD(); }

void Animator::SetVsyncWaiter(std::unique_ptr<VsyncWaiter> waiter) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_CHECK(waiter != nullptr);
  waiter_ = std::move(waiter);  // 旧 waiter 析构, 其未触发的回调被丢弃
  if (frame_scheduled_) {
    AwaitVsync();
  }
}

void Animator::RequestFrame() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (!running_) {
    pending_on_start_ = true;
    return;
  }
  if (frame_scheduled_) {
    return;
  }
  frame_scheduled_ = true;
  AwaitVsync();
}

void Animator::AwaitVsync() {
  waiter_->AsyncWaitForVsync([weak = WeakPtr<Animator>(this)](TimePoint start,
                                                                  TimePoint target) {
    if (Animator* self = weak.get()) {
      self->BeginFrame(start, target);
    }
  });
}

void Animator::BeginFrame(TimePoint frame_start, TimePoint frame_target) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (!running_) {
    frame_scheduled_ = false;
    pending_on_start_ = true;
    return;
  }

  continuation_ = pipeline_->Produce();
  if (!continuation_) {
    // raster 落后: 不驱动 JS, 保持"已请求"状态等下一次 vsync。
    stats_.frames_skipped++;
    AwaitVsync();
    return;
  }

  frame_scheduled_ = false;
  frame_start_ = frame_start;
  frame_target_ = frame_target;
  build_start_ = Clock::now();
  stats_.frames_begun++;

  current_ = std::make_unique<FrameItem>();
  delegate_.OnAnimatorBeginFrame(frame_start, frame_target);

  std::unique_ptr<FrameItem> item = std::move(current_);
  if (item->layer_trees.empty()) {
    // 本帧没有任何视图提交画面: 归还槽位。
    continuation_ = LayerTreePipeline::ProducerContinuation();
    return;
  }
  continuation_.Complete(std::move(item));
  stats_.frames_rendered++;
  delegate_.OnAnimatorDraw(pipeline_);
}

void Animator::Render(std::unique_ptr<graphics::LayerTree> layer_tree) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (layer_tree == nullptr) {
    return;
  }
  if (!continuation_ || current_ == nullptr) {
    stats_.renders_dropped++;
    MOMO_LOG(VERBOSE) << "Render outside of a frame dropped.";
    return;
  }
  for (const auto& tree : current_->layer_trees) {
    if (tree->view_id() == layer_tree->view_id()) {
      stats_.renders_dropped++;
      MOMO_LOG(VERBOSE) << "Second render for view " << layer_tree->view_id()
                        << " in one frame dropped.";
      return;
    }
  }
  graphics::LayerTree::FrameTimings& timings = layer_tree->mutable_timings();
  timings.vsync_start = frame_start_;
  timings.frame_target = frame_target_;
  timings.build_start = build_start_;
  timings.build_end = Clock::now();
  current_->layer_trees.push_back(std::move(layer_tree));
}

void Animator::Start() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (running_) {
    return;
  }
  running_ = true;
  if (pending_on_start_) {
    pending_on_start_ = false;
    RequestFrame();
  }
}

void Animator::Stop() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (!running_) {
    return;
  }
  running_ = false;
  // 已发出的 vsync 请求保留: 到达时 BeginFrame 发现已停止, 转为 pending_on_start_。
}

}  // namespace momo::engine
