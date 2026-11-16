#include "base/matrix.h"

#include <cmath>
#include <numbers>

#include "gtest/gtest.h"

namespace momo {
namespace {

TEST(MatrixTest, DefaultIsIdentity) {
  Matrix m;
  EXPECT_TRUE(m.IsIdentity());
  EXPECT_TRUE(m.IsAffine());
  EXPECT_EQ(m.MapPoint({3, 4}), (Point{3, 4}));
}

TEST(MatrixTest, TranslateAndScaleMapPoints) {
  EXPECT_EQ(Matrix::Translate(10, 20).MapPoint({1, 2}), (Point{11, 22}));
  EXPECT_EQ(Matrix::Scale(2, 3).MapPoint({1, 2}), (Point{2, 6}));
}

TEST(MatrixTest, CompositionAppliesRightOperandFirst) {
  // 先缩放再平移。
  const Matrix m = Matrix::Translate(10, 0) * Matrix::Scale(2, 2);
  EXPECT_EQ(m.MapPoint({1, 1}), (Point{12, 2}));

  Matrix pre = Matrix::Translate(10, 0);
  pre.PreConcat(Matrix::Scale(2, 2));
  EXPECT_EQ(pre, m);
}

TEST(MatrixTest, RotateZQuarterTurn) {
  const Point p = Matrix::RotateZ(std::numbers::pi / 2).MapPoint({1, 0});
  EXPECT_NEAR(p.x, 0, 1e-12);
  EXPECT_NEAR(p.y, 1, 1e-12);
}

TEST(MatrixTest, MapRectReturnsBoundingBox) {
  const Rect r = Matrix::RotateZ(std::numbers::pi / 4).MapRect({0, 0, 1, 1});
  const double h = std::sqrt(2.0) / 2;
  EXPECT_NEAR(r.x, -h, 1e-12);
  EXPECT_NEAR(r.y, 0, 1e-12);
  EXPECT_NEAR(r.width, 2 * h, 1e-12);
  EXPECT_NEAR(r.height, 2 * h, 1e-12);
}

TEST(MatrixTest, FromRowsIsRowMajor) {
  const Matrix m = Matrix::FromRows({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
  EXPECT_EQ(m.at(0, 3), 4);
  EXPECT_EQ(m.at(3, 0), 13);
}

}  // namespace
}  // namespace momo
