#include "embedder/common/platform_view_geometry.h"

#include <gtest/gtest.h>

namespace momo::embedder {
namespace {

using engine::platform::CompositionLayout;
using engine::platform::PlatformViewGeometry;

PlatformViewGeometry View(int64_t id, Rect frame) {
  PlatformViewGeometry g;
  g.view_id = id;
  g.frame = frame;
  return g;
}

const auto kAll = [](int64_t) { return true; };

TEST(PlatformViewGeometryTest, VisibleRectIsTheFrameClippedByEveryAncestorClip) {
  PlatformViewGeometry g = View(1, {0, 0, 100, 100});
  g.clips.push_back({{10, 0, 200, 200}, 0});
  g.clips.push_back({{0, 20, 50, 200}, 0});
  EXPECT_EQ(PlatformViewVisibleRect(g), (Rect{10, 20, 40, 80}));
  g.clips.push_back({{500, 500, 10, 10}, 0});
  EXPECT_EQ(PlatformViewVisibleRect(g), std::nullopt);
}

TEST(PlatformViewGeometryTest, CornerRadiusOnlyFromClipsThatFitTheVisibleRect) {
  PlatformViewGeometry g = View(1, {0, 0, 100, 100});
  g.clips.push_back({{0, 0, 100, 100.5}, 8});
  g.clips.push_back({{-50, -50, 300, 300}, 20});  // 不贴合: 不影响可见区域的圆角
  EXPECT_EQ(PlatformViewCornerRadius(g, *PlatformViewVisibleRect(g)), 8);
}

TEST(PlatformViewGeometryTest, TopmostVisibleViewReceivesUnlessMomoContentCoversIt) {
  CompositionLayout layout;
  layout.views.push_back(View(1, {0, 0, 100, 100}));
  layout.views.push_back(View(2, {50, 0, 100, 100}));
  layout.views[1].occluders.push_back({60, 0, 10, 10});  // 视图 2 之上的 momo 内容
  layout.views[0].occluders.push_back({60, 0, 10, 10});  // 同一块内容也在视图 1 之上
  EXPECT_EQ(PlatformViewAt(layout, {10, 50}, kAll), 1);
  EXPECT_EQ(PlatformViewAt(layout, {75, 50}, kAll), 2);
  EXPECT_EQ(PlatformViewAt(layout, {65, 5}, kAll), 0);
  EXPECT_EQ(PlatformViewAt(layout, {200, 50}, kAll), 0);
}

TEST(PlatformViewGeometryTest, ViewsHostedElsewhereAreSkipped) {
  CompositionLayout layout;
  layout.views.push_back(View(1, {0, 0, 100, 100}));
  layout.views.push_back(View(2, {0, 0, 100, 100}));
  EXPECT_EQ(PlatformViewAt(layout, {10, 10}, [](int64_t id) { return id != 2; }), 1);
}

}  // namespace
}  // namespace momo::embedder
