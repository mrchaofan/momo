#include "engine/shell/rasterizer.h"

#include <utility>

#include "base/logging.h"
#include "engine/graphics/composition.h"

namespace momo::engine {

namespace {

const char* BackendName(platform::SurfaceKind kind) {
  switch (kind) {
    case platform::SurfaceKind::kMetal: return "Metal";
    case platform::SurfaceKind::kDirect3D: return "Direct3D 12";
    case platform::SurfaceKind::kSoftware: return "software";
    case platform::SurfaceKind::kNone: return "null";
  }
  return "?";
}

platform::CompositionLayout MakeLayout(const graphics::Composition& composition) {
  platform::CompositionLayout layout;
  const auto& views = composition.views();
  layout.views.reserve(views.size());
  layout.overlays.reserve(views.size());
  for (size_t i = 0; i < views.size(); ++i) {
    const graphics::EmbeddedView& view = views[i];
    platform::PlatformViewGeometry geometry;
    geometry.view_id = view.view_id;
    geometry.frame = view.device_frame();
    geometry.axis_aligned = view.axis_aligned();
    geometry.opacity = view.opacity;
    for (const graphics::DeviceClip& clip : view.clips) {
      geometry.clips.push_back({clip.rect, clip.radius});
    }
    geometry.occluders = composition.OccludersAbove(i);
    layout.views.push_back(std::move(geometry));
    layout.overlays.push_back(composition.slice_bounds(i + 1));
  }
  return layout;
}

}  // namespace

Rasterizer::~Rasterizer() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  TeardownAll();
}

const Rasterizer::ViewState* Rasterizer::Find(platform::ViewId view_id) const {
  auto it = views_.find(view_id);
  return it != views_.end() ? &it->second : nullptr;
}

platform::RenderSurface* Rasterizer::surface(platform::ViewId view_id) const {
  const ViewState* view = Find(view_id);
  return view != nullptr ? view->surface : nullptr;
}

bool Rasterizer::has_backend(platform::ViewId view_id) const {
  const ViewState* view = Find(view_id);
  return view != nullptr && view->backend != nullptr;
}

platform::SurfaceKind Rasterizer::backend_kind(platform::ViewId view_id) const {
  const ViewState* view = Find(view_id);
  return view != nullptr && view->backend ? view->backend->kind() : platform::SurfaceKind::kNone;
}

const graphics::LayerTree* Rasterizer::last_layer_tree(platform::ViewId view_id) const {
  const ViewState* view = Find(view_id);
  return view != nullptr ? view->last_layer_tree.get() : nullptr;
}

void Rasterizer::Setup(platform::ViewId view_id, platform::RenderSurface* surface) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  Teardown(view_id);
  if (surface == nullptr) {
    return;
  }
  ViewState& view = views_[view_id];
  view.surface = surface;
  view.logged_acquire_failure = false;
  view.logged_no_platform_views = false;
  view.composing = false;
  view.first_frame_reported = false;
  CreateBackend(view);
  MOMO_LOG(VERBOSE) << "Rasterizer surface attached (view " << view_id << ").";
  // 绑定前已收到的最新一帧立即补画, 尽早替换闪屏。
  DrawLastLayerTree(view_id);
}

void Rasterizer::CreateBackend(ViewState& view) {
  platform::RenderSurface& surface = *view.surface;
  StatusOr<std::unique_ptr<RenderBackend>> backend = CreateRenderBackend(surface);
  if (!backend.ok() && surface.kind() != platform::SurfaceKind::kSoftware &&
      surface.kind() != platform::SurfaceKind::kNone) {
    // GPU 后端不可用(无设备、驱动/上下文创建失败): 逐级降回软件(docs/05 §4)。
    MOMO_LOG(WARNING) << "GPU render backend unavailable (" << backend.status()
                      << "); falling back to software rendering.";
    if (surface.FallBackToSoftware()) {
      backend = CreateRenderBackend(surface);
    } else {
      MOMO_LOG(ERROR) << "Rasterizer: surface does not support software fallback.";
    }
  }
  if (!backend.ok()) {
    MOMO_LOG(ERROR) << "Rasterizer: " << backend.status();
    return;
  }
  view.backend = std::move(backend).value();
  MOMO_LOG(INFO) << "Rasterizer: using " << BackendName(view.backend->kind()) << " backend.";
}

void Rasterizer::Teardown(platform::ViewId view_id) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  auto it = views_.find(view_id);
  if (it == views_.end() || it->second.surface == nullptr) {
    return;
  }
  it->second.backend.reset();  // 在 raster 线程释放 GPU 资源(GrDirectContext 等, docs/01 §4)
  it->second.surface = nullptr;
  MOMO_LOG(VERBOSE) << "Rasterizer surface detached (view " << view_id << ").";
}

void Rasterizer::TeardownAll() {
  for (auto& [id, view] : views_) {
    if (view.surface != nullptr) {
      Teardown(id);
    }
  }
}

void Rasterizer::RemoveView(platform::ViewId view_id) {
  Teardown(view_id);
  views_.erase(view_id);
}

void Rasterizer::Draw(LayerTreePipeline& pipeline) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  // 同一任务内循环消费: 不向 raster 重投自身, 避免任务排到 Shell 关闭之后。
  while (true) {
    const PipelineConsumeResult result = pipeline.Consume([this](std::unique_ptr<FrameItem> item) {
      if (item == nullptr) {
        return;
      }
      for (std::unique_ptr<graphics::LayerTree>& layer_tree : item->layer_trees) {
        if (layer_tree == nullptr) {
          continue;
        }
        ViewState& view = views_[layer_tree->view_id()];
        if (view.surface == nullptr) {
          stats_.frames_without_surface++;
        } else {
          DrawAndReport(layer_tree->view_id(), view, *layer_tree);
        }
        view.last_layer_tree = std::move(layer_tree);
      }
    });
    if (result != PipelineConsumeResult::kMoreAvailable) {
      return;
    }
  }
}

bool Rasterizer::DrawLastLayerTree(platform::ViewId view_id) {
  auto it = views_.find(view_id);
  if (it == views_.end() || it->second.last_layer_tree == nullptr || it->second.surface == nullptr) {
    return false;
  }
  DrawAndReport(view_id, it->second, *it->second.last_layer_tree);
  return true;
}

void Rasterizer::DrawAndReport(platform::ViewId view_id, ViewState& view,
                               graphics::LayerTree& layer_tree) {
  const uint64_t presented = stats_.frames_presented;
  DrawToSurface(view, layer_tree);
  if (stats_.frames_presented != presented && !view.first_frame_reported) {
    view.first_frame_reported = true;
    if (on_first_frame_) {
      on_first_frame_(view_id);
    }
  }
}

void Rasterizer::DrawToSurface(ViewState& view, graphics::LayerTree& layer_tree) {
  if (view.backend != nullptr && view.backend->kind() != view.surface->kind()) {
    // surface 自己改变了种类(GPU 设备丢失后回退软件等): 按新的种类重建后端(在 raster 线程释放旧的 GPU 资源)。
    MOMO_LOG(WARNING) << "Rasterizer: surface changed from " << BackendName(view.backend->kind()) << " to "
                      << BackendName(view.surface->kind()) << "; recreating the render backend.";
    view.backend.reset();
    CreateBackend(view);
  }
  if (view.backend == nullptr) {
    stats_.frames_failed++;
    return;
  }
  if (layer_tree.frame_size().IsEmpty()) {
    return;  // 视图尺寸尚未就绪(首个 metrics 之前): 不是失败, 等下一帧
  }
  if (layer_tree.has_platform_views() || view.composing) {
    if (view.surface->SupportsPlatformViews()) {
      DrawComposited(view, layer_tree);
      return;
    }
    view.composing = false;
    if (layer_tree.has_platform_views() && !view.logged_no_platform_views) {
      view.logged_no_platform_views = true;
      MOMO_LOG(WARNING) << "The render surface does not support platform views; they are not shown.";
    }
  }
  StatusOr<platform::SurfaceFrame> frame = view.surface->AcquireFrame(layer_tree.frame_size());
  if (!frame.ok()) {
    stats_.frames_failed++;
    if (!view.logged_acquire_failure) {
      view.logged_acquire_failure = true;  // 每次 Setup 只报一次, 避免逐帧刷屏
      MOMO_LOG(WARNING) << "AcquireFrame failed: " << frame.status();
    }
    return;
  }
  if (Status status = view.backend->Draw(layer_tree, *frame); !status.ok()) {
    stats_.frames_failed++;
    MOMO_LOG(ERROR) << "Rasterization failed: " << status;
    return;
  }
  if (frame->present && !frame->present()) {
    stats_.frames_failed++;
    return;
  }
  stats_.frames_presented++;
}

void Rasterizer::DrawComposited(ViewState& view, graphics::LayerTree& layer_tree) {
  // 本帧可能已没有平台视图("退出合成"帧): 仍走合成帧呈现, 让 surface 隐藏它们。
  std::unique_ptr<graphics::Composition> composition = graphics::Composition::Build(layer_tree);
  const platform::CompositionLayout layout = MakeLayout(*composition);
  StatusOr<platform::CompositionFrame> frame =
      view.surface->AcquireCompositionFrame(layer_tree.frame_size(), layout);
  if (!frame.ok()) {
    stats_.frames_failed++;
    if (!view.logged_acquire_failure) {
      view.logged_acquire_failure = true;
      MOMO_LOG(WARNING) << "AcquireCompositionFrame failed: " << frame.status();
    }
    return;
  }
  Status status;
  if (composition->views().empty()) {
    // 无平台视图: 整棵树按普通方式画进主帧(无需分片)。
    status = view.backend->Draw(layer_tree, frame->base);
    if (status.ok()) {
      view.backend->PrepareForExternalPresent();
    }
  } else {
    status = view.backend->DrawComposition(*composition, *frame);
  }
  if (!status.ok()) {
    stats_.frames_failed++;
    MOMO_LOG(ERROR) << "Rasterization failed: " << status;
    return;
  }
  if (frame->present && !frame->present()) {
    stats_.frames_failed++;
    return;
  }
  const bool composing = !composition->views().empty();
  if (composing != view.composing) {
    MOMO_LOG(INFO) << "Platform view composition " << (composing ? "started" : "ended") << " ("
                   << composition->views().size() << " platform view(s), "
                   << layout.overlays.size() << " overlay slot(s)).";
  }
  view.composing = composing;
  stats_.frames_presented++;
  stats_.frames_composited++;
}

}  // namespace momo::engine
