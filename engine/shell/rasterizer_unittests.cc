#include "engine/shell/rasterizer.h"

#include <memory>
#include <vector>

#include "base/testing/scoped_test_event_loop.h"
#include "engine/graphics/display_list.h"
#include "engine/graphics/layer.h"
#include "engine/shell/animator.h"
#include "engine/shell/testing/shell_test_utils.h"
#include "gtest/gtest.h"

namespace momo::engine {
namespace {

using testing::FakeSurface;

// 声称是 GPU(Metal)surface, 但给不出设备(模拟无 GPU / 设备创建失败)。
// 支持回退时委托给内部的软件 FakeSurface。
class FakeGpuSurface final : public platform::RenderSurface {
 public:
  explicit FakeGpuSurface(bool supports_fallback) : supports_fallback_(supports_fallback) {}

  platform::SurfaceKind kind() const override {
    return fell_back ? platform::SurfaceKind::kSoftware : platform::SurfaceKind::kMetal;
  }
  platform::GpuContextInfo gpu_context() const override { return {}; }  // 无设备
  bool FallBackToSoftware() override {
    fallback_calls++;
    if (supports_fallback_) {
      fell_back = true;
    }
    return supports_fallback_;
  }
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size size) override {
    if (!fell_back) {
      gpu_acquired++;
      return UnavailableError("fake gpu: no drawable");
    }
    return software.AcquireFrame(size);
  }

  FakeSurface software;
  bool fell_back = false;
  int fallback_calls = 0;
  int gpu_acquired = 0;

 private:
  bool supports_fallback_;
};

// 一帧只含一棵层树(视图 view)。
std::unique_ptr<FrameItem> Frame(std::unique_ptr<graphics::LayerTree> tree) {
  auto item = std::make_unique<FrameItem>();
  item->layer_trees.push_back(std::move(tree));
  return item;
}

void Enqueue(LayerTreePipeline& pipeline, double width, platform::ViewId view = 0) {
  auto tree = std::make_unique<graphics::LayerTree>(Size{width, 10}, 1.0);
  tree->set_view_id(view);
  ASSERT_TRUE(pipeline.Produce().Complete(Frame(std::move(tree))));
}

TEST(RasterizerTest, DrawsAndPresentsQueuedFrames) {
  momo::testing::ScopedTestEventLoop loop;  // Rasterizer 归属当前线程
  auto pipeline = LayerTreePipeline::Create(2);
  FakeSurface surface;
  Rasterizer rasterizer;
  rasterizer.Setup(&surface);

  Enqueue(*pipeline, 10);
  Enqueue(*pipeline, 20);
  rasterizer.Draw(*pipeline);  // 一次消费全部

  EXPECT_EQ(surface.acquired.load(), 2);
  EXPECT_EQ(surface.presented.load(), 2);
  EXPECT_EQ(surface.last_width.load(), 20);
  EXPECT_EQ(rasterizer.stats().frames_presented, 2u);
  EXPECT_EQ(pipeline->available_slots(), 2u);
  ASSERT_NE(rasterizer.last_layer_tree(), nullptr);
  EXPECT_EQ(rasterizer.last_layer_tree()->frame_size().width, 20);
}

TEST(RasterizerTest, FramesWithoutSurfaceAreKeptAndDrawnOnSetup) {
  momo::testing::ScopedTestEventLoop loop;  // Rasterizer 归属当前线程
  auto pipeline = LayerTreePipeline::Create(2);
  Rasterizer rasterizer;
  Enqueue(*pipeline, 30);
  rasterizer.Draw(*pipeline);
  EXPECT_EQ(rasterizer.stats().frames_without_surface, 1u);

  FakeSurface surface;
  rasterizer.Setup(&surface);  // 补画最近一帧
  EXPECT_EQ(surface.presented.load(), 1);
  EXPECT_EQ(surface.last_width.load(), 30);
  rasterizer.Teardown();
  EXPECT_EQ(rasterizer.surface(), nullptr);
  EXPECT_FALSE(rasterizer.DrawLastLayerTree());
}

TEST(RasterizerTest, AcquireFailureIsCounted) {
  momo::testing::ScopedTestEventLoop loop;  // Rasterizer 归属当前线程
  auto pipeline = LayerTreePipeline::Create(2);
  FakeSurface surface;
  surface.fail_acquire = true;
  Rasterizer rasterizer;
  rasterizer.Setup(&surface);
  Enqueue(*pipeline, 10);
  rasterizer.Draw(*pipeline);
  EXPECT_EQ(rasterizer.stats().frames_failed, 1u);
  EXPECT_EQ(surface.presented.load(), 0);
}

TEST(RasterizerTest, SoftwareSurfaceUsesSoftwareBackend) {
  momo::testing::ScopedTestEventLoop loop;
  FakeSurface surface;
  Rasterizer rasterizer;
  rasterizer.Setup(&surface);
  EXPECT_TRUE(rasterizer.has_backend());
  EXPECT_EQ(rasterizer.backend_kind(), platform::SurfaceKind::kSoftware);
  rasterizer.Teardown();
  EXPECT_FALSE(rasterizer.has_backend());  // 后端(及其 GPU 资源)随 Teardown 释放
}

TEST(RasterizerTest, GpuBackendFailureFallsBackToSoftware) {
  momo::testing::ScopedTestEventLoop loop;
  auto pipeline = LayerTreePipeline::Create(2);
  FakeGpuSurface surface(/*supports_fallback=*/true);
  Rasterizer rasterizer;
  rasterizer.Setup(&surface);

  EXPECT_EQ(surface.fallback_calls, 1);
  EXPECT_EQ(surface.kind(), platform::SurfaceKind::kSoftware);
  EXPECT_EQ(rasterizer.backend_kind(), platform::SurfaceKind::kSoftware);

  Enqueue(*pipeline, 12);
  rasterizer.Draw(*pipeline);
  EXPECT_EQ(surface.gpu_acquired, 0);
  EXPECT_EQ(surface.software.presented.load(), 1);
  EXPECT_EQ(surface.software.last_width.load(), 12);
  EXPECT_EQ(rasterizer.stats().frames_presented, 1u);
}

TEST(RasterizerTest, GpuBackendFailureWithoutFallbackDropsFrames) {
  momo::testing::ScopedTestEventLoop loop;
  auto pipeline = LayerTreePipeline::Create(2);
  FakeGpuSurface surface(/*supports_fallback=*/false);
  Rasterizer rasterizer;
  rasterizer.Setup(&surface);

  EXPECT_EQ(surface.fallback_calls, 1);
  EXPECT_FALSE(rasterizer.has_backend());
  Enqueue(*pipeline, 10);
  rasterizer.Draw(*pipeline);
  EXPECT_EQ(rasterizer.stats().frames_failed, 1u);
  EXPECT_EQ(surface.gpu_acquired, 0);  // 无后端时不取帧
  ASSERT_NE(rasterizer.last_layer_tree(), nullptr);
}

// ---- 平台视图合成路径(docs/05 §9)----

// 支持平台视图的软件 surface: 记录每次合成帧的布局, 主帧与 overlay 都是软件像素缓冲。
class FakeCompositingSurface final : public platform::RenderSurface {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  bool SupportsPlatformViews() const override { return supports; }
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size size) override {
    normal_frames++;
    return Frame(base, size);
  }
  StatusOr<platform::CompositionFrame> AcquireCompositionFrame(
      Size size, const platform::CompositionLayout& layout) override {
    layouts.push_back(layout);
    platform::CompositionFrame frame;
    frame.base = Frame(base, size);
    overlays.assign(layout.overlays.size(), {});
    frame.overlays.resize(layout.overlays.size());
    for (size_t i = 0; i < layout.overlays.size(); ++i) {
      if (layout.overlays[i]) {
        frame.overlays[i] = Frame(overlays[i], layout.overlays[i]->size());
      }
    }
    frame.present = [this] {
      composited_presents++;
      return true;
    };
    return frame;
  }

  struct Buffer {
    std::vector<uint32_t> pixels;
    int width = 0;
    // 0xAARRGGBB(仅用于不透明像素比对)。
    uint32_t At(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
  };

  bool supports = true;
  Buffer base;
  std::vector<Buffer> overlays;
  std::vector<platform::CompositionLayout> layouts;
  int normal_frames = 0;
  int composited_presents = 0;

 private:
  static platform::SurfaceFrame Frame(Buffer& buffer, Size size) {
    const int w = static_cast<int>(size.width);
    const int h = static_cast<int>(size.height);
    buffer.width = w;
    buffer.pixels.assign(static_cast<size_t>(w) * h, 0);
    platform::SurfaceFrame frame;
    frame.kind = platform::SurfaceKind::kSoftware;
    frame.software = {buffer.pixels.data(), static_cast<size_t>(w) * 4, w, h};
    frame.size = size;
    frame.present = [] { return true; };
    return frame;
  }
};

std::unique_ptr<graphics::LayerTree> TreeWithPlatformView(bool with_view) {
  auto square = [](uint32_t color) {
    graphics::DisplayListBuilder builder;
    graphics::Paint paint;
    paint.color = color;
    builder.AddHitRegion({0, 0, 10, 10});
    builder.DrawRect({0, 0, 10, 10}, paint);
    return builder.Build();
  };
  auto root = std::make_shared<graphics::ContainerLayer>();
  root->Add(std::make_shared<graphics::PictureLayer>(Point{0, 0}, square(0xFFFF0000)));
  if (with_view) {
    root->Add(std::make_shared<graphics::PlatformViewLayer>(Rect{20, 20, 30, 30}, 42));
  }
  root->Add(std::make_shared<graphics::PictureLayer>(Point{40, 40}, square(0xFF0000FF)));
  auto tree = std::make_unique<graphics::LayerTree>(Size{64, 64}, 1.0, root);
  tree->set_has_platform_views(with_view);
  return tree;
}

TEST(RasterizerTest, PlatformViewFramesAreComposited) {
  momo::testing::ScopedTestEventLoop loop;
  auto pipeline = LayerTreePipeline::Create(2);
  FakeCompositingSurface surface;
  Rasterizer rasterizer;
  rasterizer.Setup(&surface);

  ASSERT_TRUE(pipeline->Produce().Complete(Frame(TreeWithPlatformView(true))));
  rasterizer.Draw(*pipeline);
  ASSERT_EQ(surface.layouts.size(), 1u);
  const platform::CompositionLayout& layout = surface.layouts[0];
  ASSERT_EQ(layout.views.size(), 1u);
  EXPECT_EQ(layout.views[0].view_id, 42);
  EXPECT_EQ(layout.views[0].frame, (Rect{20, 20, 30, 30}));
  ASSERT_EQ(layout.overlays.size(), 1u);
  EXPECT_EQ(layout.overlays[0], (Rect{40, 40, 10, 10}));
  ASSERT_EQ(layout.views[0].occluders.size(), 1u);
  EXPECT_EQ(layout.views[0].occluders[0], (Rect{40, 40, 10, 10}));
  // 红块在主帧, 蓝块在 overlay(像素 (0,0) = 设备 (40,40))。
  EXPECT_EQ(surface.base.At(5, 5), 0xFFFF0000u);
  EXPECT_EQ(surface.base.At(45, 45), 0u);
  ASSERT_EQ(surface.overlays.size(), 1u);
  EXPECT_EQ(surface.overlays[0].At(5, 5), 0xFF0000FFu);
  EXPECT_EQ(surface.composited_presents, 1);
  EXPECT_EQ(rasterizer.stats().frames_composited, 1u);

  // 视图消失: 先走一次"退出合成"帧(空布局, 让宿主隐藏视图), 之后回到普通路径。
  ASSERT_TRUE(pipeline->Produce().Complete(Frame(TreeWithPlatformView(false))));
  rasterizer.Draw(*pipeline);
  ASSERT_EQ(surface.layouts.size(), 2u);
  EXPECT_TRUE(surface.layouts[1].views.empty());
  EXPECT_EQ(surface.base.At(45, 45), 0xFF0000FFu) << "whole tree drawn into the base frame";
  EXPECT_EQ(surface.normal_frames, 0);
  ASSERT_TRUE(pipeline->Produce().Complete(Frame(TreeWithPlatformView(false))));
  rasterizer.Draw(*pipeline);
  EXPECT_EQ(surface.layouts.size(), 2u);
  EXPECT_EQ(surface.normal_frames, 1);
  EXPECT_EQ(rasterizer.stats().frames_presented, 3u);
}

TEST(RasterizerTest, PlatformViewsAreSkippedOnSurfacesWithoutSupport) {
  momo::testing::ScopedTestEventLoop loop;
  auto pipeline = LayerTreePipeline::Create(2);
  FakeCompositingSurface surface;
  surface.supports = false;
  Rasterizer rasterizer;
  rasterizer.Setup(&surface);
  ASSERT_TRUE(pipeline->Produce().Complete(Frame(TreeWithPlatformView(true))));
  rasterizer.Draw(*pipeline);
  EXPECT_TRUE(surface.layouts.empty());
  EXPECT_EQ(surface.normal_frames, 1);
  // 视图不显示, 其余内容照常(蓝块画进主帧)。
  EXPECT_EQ(surface.base.At(45, 45), 0xFF0000FFu);
  EXPECT_EQ(rasterizer.stats().frames_presented, 1u);
}

}  // namespace
}  // namespace momo::engine
