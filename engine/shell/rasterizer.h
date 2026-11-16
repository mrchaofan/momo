#pragma once

// Rasterizer: raster 线程侧的根, 持有各视图 RenderSurface 的使用权与 GPU 后端(ARCHITECTURE.md §4.5)。
// 从 Pipeline 消费 FrameItem(一帧里各视图的 LayerTree)→ 按 LayerTree::view_id 找到视图 →
// AcquireFrame → graphics::RenderBackend::Draw → present。
// 含平台视图的帧(且 surface 支持)改走合成路径: Composition::Build 分片 → AcquireCompositionFrame →
// graphics::RenderBackend::DrawComposition → CompositionFrame::present(ARCHITECTURE.md §4.10)。
// 后端在 Setup 时按 surface 的种类与设备选取(graphics/render_backend.h);GPU 后端创建失败时调用
// surface->FallBackToSoftware() 并改用软件后端(ARCHITECTURE.md §4.4 逐级回退)。
// 多视图: 每个视图一份状态(surface、最近一帧、合成状态), 视图之间互不影响;后端按设备共享——surface 报告
// 同一 GPU 设备与队列(gpu_context())的视图共用一个后端, 即整个应用一份 GrDirectContext(一份着色器/字形/
// 纹理缓存), 而不是每个窗口一份。
// GPU 资源由后端持有, 最后一个使用它的视图解绑时(或析构时)于 raster 线程释放(ARCHITECTURE.md §3.8)。
// 线程约定: momo::Object, 归属 raster 线程(创建时的当前 EventLoop), 仅在该线程调用与析构。

#include <compare>
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
#include "engine/graphics/render_backend.h"

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

  // 视图(每次 Setup 之后)的第一帧呈现成功时回调(raster 线程)。
  void SetFirstFrameCallback(std::function<void(platform::ViewId)> callback) {
    on_first_frame_ = std::move(callback);
  }

  // 消费 Pipeline 中全部已入队的帧(深度有限, 通常 1 个)。
  void Draw(LayerTreePipeline& pipeline);
  // 重绘视图最近一帧(resize 后 / 首帧预热, ARCHITECTURE.md §4.4)。没有时返回 false。
  bool DrawLastLayerTree(platform::ViewId view_id);

  platform::RenderSurface* surface(platform::ViewId view_id) const;
  // 视图当前使用的后端(未绑定 surface 或后端创建失败时为空)。
  const graphics::RenderBackend* backend(platform::ViewId view_id) const;
  const graphics::LayerTree* last_layer_tree(platform::ViewId view_id) const;
  const Stats& stats() const { return stats_; }

 private:
  struct ViewState {
    platform::RenderSurface* surface = nullptr;
    std::shared_ptr<graphics::RenderBackend> backend;  // 与 surface 同时有效;与同设备的其他视图共用
    std::unique_ptr<graphics::LayerTree> last_layer_tree;
    bool logged_acquire_failure = false;
    bool logged_no_platform_views = false;
    // 上一帧走了合成路径且含平台视图: 下一帧即使没有平台视图也要走一次合成, 让 surface 隐藏它们。
    bool composing = false;
    bool first_frame_reported = false;  // 本次 Setup 后是否已报告第一帧
  };

  // 后端的身份: 种类 + GPU 设备与队列(软件/空后端没有设备, 各只有一个)。
  struct BackendKey {
    platform::SurfaceKind kind;
    void* device;
    void* queue;
    auto operator<=>(const BackendKey&) const = default;
  };

  const ViewState* Find(platform::ViewId view_id) const;
  // 给视图选定后端: 已有同设备的后端则共用, 否则新建;GPU 后端建不出来时让 surface 回退软件。
  void SelectBackend(ViewState& view);
  StatusOr<std::shared_ptr<graphics::RenderBackend>> AcquireBackend(platform::RenderSurface& surface);
  // 画进视图的 surface;本次 Setup 后第一次呈现成功时报告 on_first_frame_。
  void DrawAndReport(platform::ViewId view_id, ViewState& view, graphics::LayerTree& layer_tree);
  void DrawToSurface(ViewState& view, graphics::LayerTree& layer_tree);
  // 平台视图合成路径(ARCHITECTURE.md §4.10)。
  void DrawComposited(ViewState& view, graphics::LayerTree& layer_tree);

  std::map<platform::ViewId, ViewState> views_;
  // 在用的后端(视图持有强引用;没有视图使用的条目在下次选取时清掉)。后端自己持有设备的引用, 所以键里的
  // 指针在条目有效期间不会被别的设备复用。
  std::map<BackendKey, std::weak_ptr<graphics::RenderBackend>> backends_;
  Stats stats_;
  std::function<void(platform::ViewId)> on_first_frame_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Rasterizer);
};

}  // namespace momo::engine
