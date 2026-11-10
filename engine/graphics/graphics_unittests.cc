#include <cmath>
#include <memory>
#include <numbers>
#include <thread>

#include "base/testing/scoped_test_event_loop.h"
#include "engine/graphics/display_list.h"
#include "engine/graphics/layer.h"
#include "engine/graphics/layer_tree.h"
#include "engine/graphics/path.h"
#include "engine/graphics/skia/paint_context.h"
#include "engine/graphics/software_rasterizer.h"
#include "engine/graphics/texture.h"
#include "engine/graphics/testing/golden.h"
#include "gtest/gtest.h"

namespace momo::engine::graphics {
namespace {

using testing::MatchesGolden;

Paint Fill(uint32_t color) {
  Paint p;
  p.color = color;
  return p;
}

// ---- Path ----

// 规则: 回调在注册方线程执行。扩展在自己的线程推帧时, 监听者不能被直接调用。
TEST(TextureRegistryTest, FrameListenerRunsOnRegisteringThread) {
  ::momo::testing::ScopedTestEventLoop loop;
  TextureRegistry registry;
  int calls = 0;
  std::thread::id ran_on;
  registry.SetFrameListener(loop.task_runner(), [&](int64_t) {
    ran_on = std::this_thread::get_id();
    calls++;
  });
  std::shared_ptr<Texture> texture = registry.Register();
  std::thread producer([&] { registry.MarkFrameAvailable(texture->id()); });
  producer.join();
  EXPECT_EQ(calls, 0) << "listener must not run on the producer thread";
  loop.RunUntilIdle();
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(ran_on, std::this_thread::get_id());
}

TEST(PathTest, RecordsVerbsAndBounds) {
  Path path;
  path.MoveTo(10, 10).LineTo(50, 10).QuadTo(60, 30, 50, 50).Close();
  EXPECT_EQ(path.verbs().size(), 4u);
  EXPECT_EQ(path.points().size(), 4u);
  EXPECT_EQ(path.bounds(), (Rect{10, 10, 50, 40}));
}

TEST(PathTest, DrawingAfterCloseStartsAtLastMove) {
  Path path;
  path.MoveTo(5, 5).LineTo(10, 5).Close().LineTo(20, 20);
  ASSERT_EQ(path.verbs()[3], Path::Verb::kMove);  // 隐式 MoveTo(5,5)
  EXPECT_EQ(path.points()[2], (Point{5, 5}));
}

// ---- DisplayList ----

TEST(DisplayListTest, BoundsFollowTransformClipAndStroke) {
  DisplayListBuilder builder;
  builder.Translate(100, 0);
  builder.DrawRect({0, 0, 10, 10}, Fill(0xFFFF0000));
  Paint stroke = Fill(0xFF00FF00);
  stroke.style = Paint::Style::kStroke;
  stroke.stroke_width = 4;
  builder.DrawRect({0, 20, 10, 10}, stroke);  // 外扩 4
  auto list = builder.Build();
  ASSERT_TRUE(list->bounds().has_value());
  EXPECT_EQ(*list->bounds(), (Rect{96, 0, 18, 34}));

  DisplayListBuilder clipped;
  clipped.ClipRect({0, 0, 5, 5});
  clipped.DrawRect({0, 0, 100, 100}, Fill(0xFF000000));
  EXPECT_EQ(*clipped.Build()->bounds(), (Rect{0, 0, 5, 5}));
}

TEST(DisplayListTest, UnboundedOpsUseCullRectOrBecomeUnbounded) {
  DisplayListBuilder unbounded;
  unbounded.DrawColor(0xFFFFFFFF);
  EXPECT_FALSE(unbounded.Build()->bounds().has_value());

  DisplayListBuilder culled(Rect{0, 0, 64, 32});
  culled.DrawPaint(Fill(0xFF0000FF));
  EXPECT_EQ(*culled.Build()->bounds(), (Rect{0, 0, 64, 32}));
}

TEST(DisplayListTest, BuildBalancesSavesAndResetsBuilder) {
  DisplayListBuilder builder;
  builder.Save();
  builder.Save();
  builder.Translate(1, 1);
  builder.Restore();
  builder.Restore();
  builder.Restore();  // 多余, 忽略
  builder.Save();     // 未配平
  auto list = builder.Build();
  int depth = 0;
  for (const auto& op : list->ops()) {
    depth += op.type == DisplayList::OpType::kSave ? 1 : 0;
    depth -= op.type == DisplayList::OpType::kRestore ? 1 : 0;
    EXPECT_GE(depth, 0);
  }
  EXPECT_EQ(depth, 0);
  EXPECT_EQ(builder.save_count(), 0);
  EXPECT_EQ(builder.Build()->op_count(), 0u) << "builder reusable after Build";
}

TEST(DisplayListTest, ConsecutiveIdenticalPaintsAreShared) {
  DisplayListBuilder builder;
  const Paint red = Fill(0xFFFF0000);
  builder.DrawRect({0, 0, 1, 1}, red);
  builder.DrawRect({1, 0, 1, 1}, red);
  builder.DrawCircle({5, 5}, 1, Fill(0xFF00FF00));
  EXPECT_EQ(builder.Build()->paints().size(), 2u);
}

// ---- Layer ----

TEST(DisplayListTest, RasterCacheIsBuiltOnceAndReusedByParents) {
  DisplayListBuilder child_builder;
  child_builder.DrawRect({0, 0, 10, 10}, Paint{});
  auto child = child_builder.Build();
  const SkPicture* picture = child->raster_cache().picture.get();
  ASSERT_NE(picture, nullptr);
  EXPECT_EQ(child->raster_cache().picture.get(), picture) << "second access must hit the cache";

  DisplayListBuilder parent_builder;
  parent_builder.DrawDisplayList(child);
  auto parent = parent_builder.Build();
  ASSERT_NE(parent->raster_cache().picture, nullptr);
  EXPECT_EQ(child->raster_cache().picture.get(), picture);
}

TEST(LayerTest, PrerollComputesParentSpaceBounds) {
  DisplayListBuilder builder;
  builder.DrawRect({0, 0, 10, 10}, Fill(0xFF000000));
  auto picture = builder.Build();

  auto root = std::make_shared<ContainerLayer>();
  auto offset = std::make_shared<OffsetLayer>(Point{20, 30});
  offset->Add(std::make_shared<PictureLayer>(Point{5, 5}, picture));
  auto transform = std::make_shared<TransformLayer>(Matrix::Scale(2, 2));
  transform->Add(std::make_shared<PictureLayer>(Point{0, 0}, picture));
  root->Add(offset);
  root->Add(transform);

  PrerollContext context;
  root->Preroll(context);
  EXPECT_EQ(*offset->paint_bounds(), (Rect{25, 35, 10, 10}));
  EXPECT_EQ(*transform->paint_bounds(), (Rect{0, 0, 20, 20}));
  EXPECT_EQ(*root->paint_bounds(), (Rect{0, 0, 35, 45}));
}

// ---- 软件光栅 ----

TEST(SoftwareRasterizerTest, RejectsInvalidBuffer) {
  LayerTree tree({10, 10}, 1.0);
  EXPECT_FALSE(RasterizeLayerTree(tree, PixelBuffer{}).ok());
}

TEST(SoftwareRasterizerTest, EmptyTreeClearsToClearColor) {
  OwnedPixelBuffer pixels(8, 8);
  LayerTree tree({8, 8}, 1.0);
  ASSERT_TRUE(RasterizeLayerTree(tree, pixels.view(), 0xFF336699).ok());
  EXPECT_EQ(pixels.GetColor(4, 4), 0xFF336699u);
}

TEST(SoftwareRasterizerTest, LayersComposeOffsetsClipsAndOpacity) {
  DisplayListBuilder builder;
  builder.DrawRect({0, 0, 20, 20}, Fill(0xFFFF0000));
  auto red_square = builder.Build();

  auto root = std::make_shared<ContainerLayer>();
  auto offset = std::make_shared<OffsetLayer>(Point{10, 10});
  offset->Add(std::make_shared<PictureLayer>(Point{0, 0}, red_square));
  auto clip = std::make_shared<ClipRectLayer>(Rect{40, 0, 10, 10});
  clip->Add(std::make_shared<PictureLayer>(Point{40, 0}, red_square));
  auto opacity = std::make_shared<OpacityLayer>(128);
  opacity->Add(std::make_shared<PictureLayer>(Point{0, 40}, red_square));
  root->Add(offset);
  root->Add(clip);
  root->Add(opacity);

  OwnedPixelBuffer pixels(64, 64);
  LayerTree tree({64, 64}, 1.0, root);
  ASSERT_TRUE(RasterizeLayerTree(tree, pixels.view(), 0xFFFFFFFF).ok());
  EXPECT_EQ(pixels.GetColor(15, 15), 0xFFFF0000u);  // offset 内
  EXPECT_EQ(pixels.GetColor(5, 5), 0xFFFFFFFFu);    // offset 外
  EXPECT_EQ(pixels.GetColor(45, 5), 0xFFFF0000u);   // clip 内
  EXPECT_EQ(pixels.GetColor(45, 15), 0xFFFFFFFFu);  // 被 clip 掉
  const uint32_t half = pixels.GetColor(10, 50);     // 50% 红叠白
  EXPECT_NEAR(static_cast<int>((half >> 8) & 0xFF), 127, 2);
  EXPECT_EQ((half >> 16) & 0xFF, 0xFFu);
}

TEST(SoftwareRasterizerTest, PngRoundTrip) {
  OwnedPixelBuffer pixels(4, 2);
  LayerTree tree({4, 2}, 1.0);
  ASSERT_TRUE(RasterizeLayerTree(tree, pixels.view(), 0xFF102030).ok());
  auto png = EncodePng(pixels.view());
  ASSERT_TRUE(png.ok());
  OwnedPixelBuffer decoded(0, 0);
  ASSERT_TRUE(DecodePng(*png, &decoded).ok());
  EXPECT_EQ(decoded.width(), 4);
  EXPECT_EQ(decoded.GetColor(3, 1), 0xFF102030u);
}

// ---- Golden: M1 验收画面(渐变矩形 + 路径 + 描边/圆角/圆), 与 JS 示例一致 ----

std::shared_ptr<ContainerLayer> BuildAcceptanceScene() {
  DisplayListBuilder builder;
  builder.DrawColor(0xFFF5F5F7);

  Paint gradient;
  auto g = std::make_shared<Gradient>();
  g->type = Gradient::Type::kLinear;
  g->start = {20, 20};
  g->end = {220, 120};
  g->colors = {0xFF4285F4, 0xFFEA4335};
  gradient.gradient = g;
  builder.DrawRRect({20, 20, 200, 100}, 16, gradient);

  Path star;
  const double cx = 320, cy = 70, outer = 50, inner = 20;
  for (int i = 0; i < 10; ++i) {
    const double r = (i % 2 == 0) ? outer : inner;
    const double a = -std::numbers::pi / 2 + i * std::numbers::pi / 5;
    if (i == 0) {
      star.MoveTo(cx + r * std::cos(a), cy + r * std::sin(a));
    } else {
      star.LineTo(cx + r * std::cos(a), cy + r * std::sin(a));
    }
  }
  star.Close();
  builder.DrawPath(star, Fill(0xFFFBBC05));

  Paint stroke = Fill(0xFF34A853);
  stroke.style = Paint::Style::kStroke;
  stroke.stroke_width = 6;
  stroke.stroke_cap = Paint::StrokeCap::kRound;
  builder.DrawLine({20, 160}, {380, 160}, stroke);
  builder.DrawCircle({80, 220}, 30, stroke);

  builder.Save();
  builder.Translate(220, 220);
  builder.Rotate(std::numbers::pi / 4);
  builder.DrawRect({-25, -25, 50, 50}, Fill(0xCC9C27B0));
  builder.Restore();

  auto root = std::make_shared<ContainerLayer>();
  root->Add(std::make_shared<PictureLayer>(Point{0, 0}, builder.Build()));
  return root;
}

TEST(GoldenTest, AcceptanceScene) {
  OwnedPixelBuffer pixels(400, 270);
  LayerTree tree({400, 270}, 1.0, BuildAcceptanceScene());
  ASSERT_TRUE(RasterizeLayerTree(tree, pixels.view()).ok());
  EXPECT_TRUE(MatchesGolden("acceptance_scene", pixels));
}

}  // namespace
}  // namespace momo::engine::graphics
