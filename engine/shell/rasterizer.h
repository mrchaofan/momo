#pragma once

// Rasterizer: raster 线程侧的根, 持有各视图 RenderSurface 的使用权与 GPU 后端(docs/05)。
// 从 Pipeline 消费 FrameItem(一帧里各视图的 LayerTree)→ 按 LayerTree::view_id 找到视图 →
// AcquireFrame → RenderBackend::Draw → present。
// 含平台视图的帧(且 surface 支持)改走合成路径: Composition::Build 分片 → AcquireCompositionFrame →
// RenderBackend::DrawComposition → CompositionFrame::present(docs/05 §9)。
// 后端在 Setup 时按 surface 种类选取(render_backend.h);GPU 后端创建失败时调用
// surface->FallBackToSoftware() 并改用软件后端(docs/05 §4 逐级回退)。
// 多视图: 每个视图一份状态(surface、后端、最近一帧、合成状态);视图之间互不影响。
// GPU 资源(GrDirectContext)由后端持有, 在 Teardown / 析构时于 raster 线程释放(docs/01 §4)。
// 线程约定: momo::Object, 归属 raster 线程(创建时的当前 EventLoop), 仅在该线程调用与析构。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>

#include "base/macros.h"
#include "base/object.h"
#include "base/pipeline.h"
#include "engine/graphics/layer_tree.h"
#include "engine/platform/render_surface.h"
#include "engine/platform/view.h"
#include "engine/shell/animator.h"
#include "engine/shell/render_backend.h"

namespace momo::engine {

class Rasterizer final : public Object {
 public:
  struct Stats {
    uint64_t frames_presented = 0;
    uint64_t frames_failed = 0;          // 无后端、AcquireFrame、绘制或 present 失败
    uint64_t frames_without_surface = 0; // 视图未绑定 surface 时收到的层树(丢弃, 但保留为 last)
    uint64_t frames_composited = 0;      // 走平台视图合成路径并呈现成功的帧(含"退出合成"帧)
  };

  Rasterizer() = default;  // 归属当前线程的 EventLoop
  ~Rasterizer() override;

  // 绑定视图的 surface。surface 由窗口拥有;Teardown 返回后 Rasterizer 不再触碰它(GPU 资源亦已释放)。
  void Setup(platform::ViewId view_id, platform::RenderSurface* surface);
  // 解绑视图(保留其最近一帧, 重新绑定时补画)。
  void Teardown(platform::ViewId view_id);
  // 解绑全部视图。
  void TeardownAll();
  // 视图被移除: 解绑并丢弃其最近一帧。
  void RemoveView(platform::ViewId view_id);

  // 主窗口(视图 0)的兼容入口。
  void Setup(platform::RenderSurface* surface) { Setup(platform::kImplicitViewId, surface); }
  void Teardown() { Teardown(platform::kImplicitViewId); }

  // 视图(每次 Setup 之后)的第一帧呈现成功时回调(raster 线程)。
  void SetFirstFrameCallback(std::function<void(platform::ViewId)> callback) {
    on_first_frame_ = std::move(callback);
  }

  // 消费 Pipeline 中全部已入队的帧(深度有限, 通常 1 个)。
  void Draw(LayerTreePipeline& pipeline);
  // 重绘视图最近一帧(resize 后 / 首帧预热, docs/05 §5)。没有时返回 false。
  bool DrawLastLayerTree(platform::ViewId view_id = platform::kImplicitViewId);

  platform::RenderSurface* surface(platform::ViewId view_id = platform::kImplicitViewId) const;
  bool has_backend(platform::ViewId view_id = platform::kImplicitViewId) const;
  platform::SurfaceKind backend_kind(platform::ViewId view_id = platform::kImplicitViewId) const;
  const graphics::LayerTree* last_layer_tree(
      platform::ViewId view_id = platform::kImplicitViewId) const;
  const Stats& stats() const { return stats_; }

 private:
  struct ViewState {
    platform::RenderSurface* surface = nullptr;
    std::unique_ptr<RenderBackend> backend;  // 与 surface 同时有效
    std::unique_ptr<graphics::LayerTree> last_layer_tree;
    bool logged_acquire_failure = false;
    bool logged_no_platform_views = false;
    // 上一帧走了合成路径且含平台视图: 下一帧即使没有平台视图也要走一次合成, 让 surface 隐藏它们。
    bool composing = false;
    bool first_frame_reported = false;  // 本次 Setup 后是否已报告第一帧
  };

  const ViewState* Find(platform::ViewId view_id) const;
  void CreateBackend(ViewState& view);
  // 画进视图的 surface;本次 Setup 后第一次呈现成功时报告 on_first_frame_。
  void DrawAndReport(platform::ViewId view_id, ViewState& view, graphics::LayerTree& layer_tree);
  void DrawToSurface(ViewState& view, graphics::LayerTree& layer_tree);
  // 平台视图合成路径(docs/05 §9)。
  void DrawComposited(ViewState& view, graphics::LayerTree& layer_tree);

  std::map<platform::ViewId, ViewState> views_;
  Stats stats_;
  std::function<void(platform::ViewId)> on_first_frame_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Rasterizer);
};

}  // namespace momo::engine
