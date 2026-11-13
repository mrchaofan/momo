// 平台视图分片合成(docs/05 §9): Composition::Build 的收集、分片、范围与遮挡计算。

#include "engine/graphics/composition.h"

#include <memory>

#include "engine/graphics/display_list.h"
#include "engine/graphics/layer.h"
#include "engine/graphics/layer_tree.h"
#include "engine/graphics/software_rasterizer.h"
#include "gtest/gtest.h"

namespace momo::engine::graphics {
namespace {

Paint Fill(uint32_t color) {
  Paint paint;
  paint.color = color;
  return paint;
}

// 可命中的色块(框架里自身可命中的节点会同时记录可命中区域)。
std::shared_ptr<const DisplayList> Square(const Rect& rect, uint32_t color) {
  DisplayListBuilder builder;
  builder.AddHitRegion(rect);
  builder.DrawRect(rect, Fill(color));
  return builder.Build();
}

TEST(CompositionTest, TreeWithoutPlatformViewsRecordsNothing) {
  auto root = std::make_shared<ContainerLayer>();
  root->Add(std::make_shared<PictureLayer>(Point{0, 0}, Square({0, 0, 10, 10}, 0xFFFF0000)));
  LayerTree tree({50, 50}, 1.0, root);
  auto composition = Composition::Build(tree);
  EXPECT_TRUE(composition->views().empty());
  EXPECT_EQ(composition->slice_count(), 0u);
}

// 红块(视图之前)→ 平台视图 → 蓝块(视图之后): 红块进底层分片, 蓝块进视图之上的分片。
TEST(CompositionTest, SplitsLeavesAroundThePlatformView) {
  auto root = std::make_shared<ContainerLayer>();
  root->Add(std::make_shared<PictureLayer>(Point{0, 0}, Square({0, 0, 20, 20}, 0xFFFF0000)));
  auto offset = std::make_shared<OffsetLayer>(Point{10, 10});
  offset->Add(std::make_shared<PlatformViewLayer>(Rect{0, 0, 40, 40}, 5));
  root->Add(offset);
  root->Add(std::make_shared<PictureLayer>(Point{30, 30}, Square({0, 0, 20, 20}, 0xFF0000FF)));
  LayerTree tree({100, 100}, 1.0, root);
  tree.set_has_platform_views(true);

  auto composition = Composition::Build(tree);
  ASSERT_EQ(composition->views().size(), 1u);
  const EmbeddedView& view = composition->views()[0];
  EXPECT_EQ(view.view_id, 5);
  EXPECT_EQ(view.device_frame(), (Rect{10, 10, 40, 40}));
  EXPECT_TRUE(view.axis_aligned());
  ASSERT_EQ(composition->slice_count(), 2u);
  EXPECT_EQ(composition->slice_bounds(0), (Rect{0, 0, 20, 20}));
  EXPECT_EQ(composition->slice_bounds(1), (Rect{30, 30, 20, 20}));

  // 蓝块盖住视图的右下角: 那里的点击归 momo。
  const std::vector<Rect> occluders = composition->OccludersAbove(0);
  ASSERT_EQ(occluders.size(), 1u);
  EXPECT_EQ(occluders[0], (Rect{30, 30, 20, 20}));

  // 底层分片: 只有红块。
  OwnedPixelBuffer base(100, 100);
  ASSERT_TRUE(composition->RasterizeSlice(0, base.view(), {0, 0}).ok());
  EXPECT_EQ(base.GetColor(5, 5), 0xFFFF0000u);
  EXPECT_EQ(base.GetColor(35, 35) >> 24, 0u) << "blue belongs to the overlay";
  // overlay 分片按自身范围光栅: 像素 (0,0) = 设备 (30,30)。
  OwnedPixelBuffer overlay(20, 20);
  ASSERT_TRUE(composition->RasterizeSlice(1, overlay.view(), {30, 30}).ok());
  EXPECT_EQ(overlay.GetColor(5, 5), 0xFF0000FFu);
  EXPECT_FALSE(composition->RasterizeSlice(2, overlay.view(), {0, 0}).ok());
}

// 祖先变换、圆角裁剪、透明度都被记录到视图上(设备空间)。
TEST(CompositionTest, CollectsTransformClipsAndOpacity) {
  auto root = std::make_shared<ContainerLayer>();
  auto scale = std::make_shared<TransformLayer>(Matrix::Scale(2, 2));
  auto clip = std::make_shared<ClipRRectLayer>(Rect{0, 0, 30, 30}, 4);
  auto opacity = std::make_shared<OpacityLayer>(128);
  opacity->Add(std::make_shared<PlatformViewLayer>(Rect{5, 5, 50, 50}, 9));
  clip->Add(opacity);
  scale->Add(clip);
  root->Add(scale);
  LayerTree tree({200, 200}, 2.0, root);

  auto composition = Composition::Build(tree);
  ASSERT_EQ(composition->views().size(), 1u);
  const EmbeddedView& view = composition->views()[0];
  EXPECT_EQ(view.device_frame(), (Rect{10, 10, 100, 100}));
  ASSERT_EQ(view.clips.size(), 1u);
  EXPECT_EQ(view.clips[0].rect, (Rect{0, 0, 60, 60}));
  EXPECT_DOUBLE_EQ(view.clips[0].radius, 8);
  EXPECT_NEAR(view.opacity, 128 / 255.0, 1e-9);
  EXPECT_EQ(composition->VisibleFrame(0), (Rect{10, 10, 50, 50}));
  // 没有任何内容画在视图之后。
  EXPECT_FALSE(composition->slice_bounds(1).has_value());
  EXPECT_TRUE(composition->OccludersAbove(0).empty());
}

TEST(CompositionTest, RotationIsNotAxisAligned) {
  auto root = std::make_shared<ContainerLayer>();
  auto rotate = std::make_shared<TransformLayer>(Matrix::RotateZ(0.3));
  rotate->Add(std::make_shared<PlatformViewLayer>(Rect{0, 0, 10, 10}, 1));
  root->Add(rotate);
  LayerTree tree({50, 50}, 1.0, root);
  auto composition = Composition::Build(tree);
  ASSERT_EQ(composition->views().size(), 1u);
  EXPECT_FALSE(composition->views()[0].axis_aligned());
}

// 视图之后的内容受同一祖先裁剪约束(N 路画布把裁剪同步到所有分片)。
TEST(CompositionTest, AncestorClipAppliesToLaterSlices) {
  auto root = std::make_shared<ContainerLayer>();
  auto clip = std::make_shared<ClipRectLayer>(Rect{0, 0, 50, 50});
  clip->Add(std::make_shared<PlatformViewLayer>(Rect{0, 0, 40, 40}, 3));
  clip->Add(std::make_shared<PictureLayer>(Point{0, 0}, Square({0, 0, 100, 100}, 0xFF00FF00)));
  root->Add(clip);
  LayerTree tree({100, 100}, 1.0, root);
  auto composition = Composition::Build(tree);
  ASSERT_EQ(composition->slice_count(), 2u);
  EXPECT_EQ(composition->slice_bounds(1), (Rect{0, 0, 50, 50}));
  OwnedPixelBuffer overlay(50, 50);
  ASSERT_TRUE(composition->RasterizeSlice(1, overlay.view(), {0, 0}).ok());
  EXPECT_EQ(overlay.GetColor(45, 45), 0xFF00FF00u);
}

// 遮挡只看可命中区域, 不看绘制: 不可命中的绘制(阴影、IgnorePointer 下的内容)不遮挡, 只有命中区域(透明的
// 手势层)也遮挡;区域经变换与裁剪换算到设备空间, 嵌套的 DisplayList 一并计入。
TEST(CompositionTest, OccludersComeFromHitRegionsNotDrawing) {
  DisplayListBuilder inner;
  inner.AddHitRegion({0, 0, 30, 30});
  auto nested = inner.Build();

  DisplayListBuilder above;
  above.DrawRect({0, 0, 50, 50}, Fill(0x80000000));  // 只绘制
  above.Save();
  above.Translate(5, 5);
  above.ClipRect({0, 0, 10, 10});
  above.AddHitRegion({0, 0, 20, 20});  // 被裁剪为 (5,5,10,10)
  above.Restore();
  above.Translate(60, 60);
  above.DrawDisplayList(nested);  // (60,60,30,30)

  auto root = std::make_shared<ContainerLayer>();
  root->Add(std::make_shared<PlatformViewLayer>(Rect{0, 0, 100, 100}, 1));
  root->Add(std::make_shared<PictureLayer>(Point{0, 0}, above.Build()));
  LayerTree tree({100, 100}, 1.0, root);
  auto composition = Composition::Build(tree);
  EXPECT_EQ(composition->OccludersAbove(0), (std::vector<Rect>{{5, 5, 10, 10}, {60, 60, 30, 30}}))
      << "drawing alone does not occlude";
}

// 两个视图: 三个分片, 遮挡只算视图之上的分片。
TEST(CompositionTest, TwoViewsProduceThreeSlices) {
  auto root = std::make_shared<ContainerLayer>();
  root->Add(std::make_shared<PlatformViewLayer>(Rect{0, 0, 50, 50}, 1));
  root->Add(std::make_shared<PictureLayer>(Point{40, 40}, Square({0, 0, 20, 20}, 0xFFFF0000)));
  root->Add(std::make_shared<PlatformViewLayer>(Rect{45, 45, 50, 50}, 2));
  root->Add(std::make_shared<PictureLayer>(Point{80, 0}, Square({0, 0, 10, 10}, 0xFF0000FF)));
  LayerTree tree({100, 100}, 1.0, root);
  auto composition = Composition::Build(tree);
  ASSERT_EQ(composition->views().size(), 2u);
  ASSERT_EQ(composition->slice_count(), 3u);
  EXPECT_FALSE(composition->slice_bounds(0).has_value());
  EXPECT_EQ(composition->slice_bounds(1), (Rect{40, 40, 20, 20}));
  EXPECT_EQ(composition->slice_bounds(2), (Rect{80, 0, 10, 10}));
  // 视图 1 被红块(分片 1)遮住一角;蓝块不与它相交。
  const std::vector<Rect> above_first = composition->OccludersAbove(0);
  ASSERT_EQ(above_first.size(), 1u);
  EXPECT_EQ(above_first[0], (Rect{40, 40, 10, 10}));
  // 视图 2 之上只有蓝块(不相交)。红块在它之下, 不算遮挡。
  EXPECT_TRUE(composition->OccludersAbove(1).empty());
}

// 与右键菜单相同的结构: 根缩放(dpr) → 底层内容 → 平台视图 → 半透明遮罩(OpacityLayer) + 菜单。
TEST(CompositionTest, TranslucentLayersAboveTheViewReachTheOverlay) {
  auto root = std::make_shared<ContainerLayer>();
  auto dpr = std::make_shared<TransformLayer>(Matrix::Scale(2, 2));
  dpr->Add(std::make_shared<PictureLayer>(Point{0, 0}, Square({0, 0, 50, 50}, 0xFFFFFFFF)));
  dpr->Add(std::make_shared<PlatformViewLayer>(Rect{0, 10, 50, 40}, 1));
  auto dim = std::make_shared<OpacityLayer>(128);
  dim->Add(std::make_shared<PictureLayer>(Point{0, 0}, Square({0, 0, 50, 50}, 0xFF000000)));
  dpr->Add(dim);
  auto menu_opacity = std::make_shared<OpacityLayer>(200);
  auto menu_offset = std::make_shared<OffsetLayer>(Point{5, 5});
  menu_offset->Add(std::make_shared<PictureLayer>(Point{0, 0}, Square({0, 0, 20, 10}, 0xFFFF0000)));
  menu_offset->Add(std::make_shared<PictureLayer>(Point{0, 12}, Square({0, 0, 20, 10}, 0xFF00FF00)));
  menu_opacity->Add(menu_offset);
  dpr->Add(menu_opacity);
  root->Add(dpr);
  LayerTree tree({100, 100}, 2.0, root);

  auto composition = Composition::Build(tree);
  ASSERT_EQ(composition->slice_count(), 2u);
  ASSERT_TRUE(composition->slice_bounds(1).has_value());
  EXPECT_EQ(*composition->slice_bounds(1), (Rect{0, 0, 100, 100}));
  OwnedPixelBuffer overlay(100, 100);
  ASSERT_TRUE(composition->RasterizeSlice(1, overlay.view(), {0, 0}).ok());
  const uint32_t dim_px = overlay.GetColor(90, 90);  // 只有遮罩
  EXPECT_NEAR(static_cast<int>(dim_px >> 24), 128, 3) << std::hex << dim_px;
  const uint32_t menu_px = overlay.GetColor(20, 15);  // 遮罩 + 红色菜单项
  EXPECT_GT(static_cast<int>((menu_px >> 16) & 0xFF), 150) << std::hex << menu_px;
}

}  // namespace
}  // namespace momo::engine::graphics
