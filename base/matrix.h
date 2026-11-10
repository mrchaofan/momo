#pragma once

// Matrix: 4x4 仿射/透视变换矩阵(TransformLayer、hit test 用, docs/05 §3)。
// header-only, 属于 momo_base。值类型, 线程无关。
//
// 约定: 行主序存储 m[row][col], 作用于列向量 p' = M * p。
// 组合: (A * B) 表示先应用 B 再应用 A(与 Skia SkM44 一致)。

#include <array>
#include <cmath>
#include <optional>

#include "base/geometry.h"

namespace momo {

class Matrix {
 public:
  // 默认构造为单位矩阵。
  constexpr Matrix() : m_{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}} {}

  static constexpr Matrix Identity() { return Matrix(); }

  static constexpr Matrix Translate(double tx, double ty, double tz = 0) {
    Matrix r;
    r.m_[0][3] = tx;
    r.m_[1][3] = ty;
    r.m_[2][3] = tz;
    return r;
  }

  static constexpr Matrix Scale(double sx, double sy, double sz = 1) {
    Matrix r;
    r.m_[0][0] = sx;
    r.m_[1][1] = sy;
    r.m_[2][2] = sz;
    return r;
  }

  // 绕 Z 轴旋转(屏幕坐标系 y 向下时为顺时针)。
  static Matrix RotateZ(double radians) {
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Matrix r;
    r.m_[0][0] = c;
    r.m_[0][1] = -s;
    r.m_[1][0] = s;
    r.m_[1][1] = c;
    return r;
  }

  // 按行给出 16 个元素。
  static constexpr Matrix FromRows(const std::array<double, 16>& v) {
    Matrix r;
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        r.m_[i][j] = v[i * 4 + j];
      }
    }
    return r;
  }

  constexpr double at(int row, int col) const { return m_[row][col]; }
  constexpr void set(int row, int col, double value) { m_[row][col] = value; }

  constexpr bool IsIdentity() const { return *this == Matrix(); }

  // 只含 2D 平移(可用 OffsetLayer 快路径)。
  constexpr bool IsTranslateOnly() const {
    Matrix t = *this;
    t.m_[0][3] = 0;
    t.m_[1][3] = 0;
    return t.IsIdentity();
  }

  // 无透视分量(第 4 行为 0 0 0 1)。
  constexpr bool IsAffine() const {
    return m_[3][0] == 0 && m_[3][1] == 0 && m_[3][2] == 0 && m_[3][3] == 1;
  }

  friend constexpr Matrix operator*(const Matrix& a, const Matrix& b) {
    Matrix r;
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        double sum = 0;
        for (int k = 0; k < 4; ++k) {
          sum += a.m_[i][k] * b.m_[k][j];
        }
        r.m_[i][j] = sum;
      }
    }
    return r;
  }

  Matrix& operator*=(const Matrix& other) { return *this = *this * other; }

  // 先应用 other 再应用 this。
  Matrix& PreConcat(const Matrix& other) { return *this = *this * other; }
  // 先应用 this 再应用 other。
  Matrix& PostConcat(const Matrix& other) { return *this = other * *this; }

  friend constexpr bool operator==(const Matrix&, const Matrix&) = default;

  // 映射 z=0 平面上的点(含透视除法)。w 为 0 时返回原点。
  Point MapPoint(Point p) const {
    const double x = m_[0][0] * p.x + m_[0][1] * p.y + m_[0][3];
    const double y = m_[1][0] * p.x + m_[1][1] * p.y + m_[1][3];
    const double w = m_[3][0] * p.x + m_[3][1] * p.y + m_[3][3];
    if (w == 0) {
      return {};
    }
    return {x / w, y / w};
  }

  // 映射矩形, 返回四个角点的轴对齐包围盒。
  Rect MapRect(const Rect& r) const {
    const Point corners[4] = {MapPoint({r.x, r.y}),
                              MapPoint({r.x + r.width, r.y}),
                              MapPoint({r.x, r.y + r.height}),
                              MapPoint({r.x + r.width, r.y + r.height})};
    double min_x = corners[0].x, max_x = corners[0].x;
    double min_y = corners[0].y, max_y = corners[0].y;
    for (const Point& c : corners) {
      min_x = std::fmin(min_x, c.x);
      max_x = std::fmax(max_x, c.x);
      min_y = std::fmin(min_y, c.y);
      max_y = std::fmax(max_y, c.y);
    }
    return {min_x, min_y, max_x - min_x, max_y - min_y};
  }

  double Determinant() const {
    const auto& a = m_;
    const double s0 = a[0][0] * a[1][1] - a[1][0] * a[0][1];
    const double s1 = a[0][0] * a[1][2] - a[1][0] * a[0][2];
    const double s2 = a[0][0] * a[1][3] - a[1][0] * a[0][3];
    const double s3 = a[0][1] * a[1][2] - a[1][1] * a[0][2];
    const double s4 = a[0][1] * a[1][3] - a[1][1] * a[0][3];
    const double s5 = a[0][2] * a[1][3] - a[1][2] * a[0][3];
    const double c5 = a[2][2] * a[3][3] - a[3][2] * a[2][3];
    const double c4 = a[2][1] * a[3][3] - a[3][1] * a[2][3];
    const double c3 = a[2][1] * a[3][2] - a[3][1] * a[2][2];
    const double c2 = a[2][0] * a[3][3] - a[3][0] * a[2][3];
    const double c1 = a[2][0] * a[3][2] - a[3][0] * a[2][2];
    const double c0 = a[2][0] * a[3][1] - a[3][0] * a[2][1];
    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
  }

  // 逆矩阵;奇异(行列式为 0 或非有限)时返回 nullopt。
  std::optional<Matrix> Invert() const {
    const auto& a = m_;
    const double s0 = a[0][0] * a[1][1] - a[1][0] * a[0][1];
    const double s1 = a[0][0] * a[1][2] - a[1][0] * a[0][2];
    const double s2 = a[0][0] * a[1][3] - a[1][0] * a[0][3];
    const double s3 = a[0][1] * a[1][2] - a[1][1] * a[0][2];
    const double s4 = a[0][1] * a[1][3] - a[1][1] * a[0][3];
    const double s5 = a[0][2] * a[1][3] - a[1][2] * a[0][3];
    const double c5 = a[2][2] * a[3][3] - a[3][2] * a[2][3];
    const double c4 = a[2][1] * a[3][3] - a[3][1] * a[2][3];
    const double c3 = a[2][1] * a[3][2] - a[3][1] * a[2][2];
    const double c2 = a[2][0] * a[3][3] - a[3][0] * a[2][3];
    const double c1 = a[2][0] * a[3][2] - a[3][0] * a[2][2];
    const double c0 = a[2][0] * a[3][1] - a[3][0] * a[2][1];
    const double det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (det == 0 || !std::isfinite(det)) {
      return std::nullopt;
    }
    const double inv = 1.0 / det;
    Matrix r;
    auto& b = r.m_;
    b[0][0] = (a[1][1] * c5 - a[1][2] * c4 + a[1][3] * c3) * inv;
    b[0][1] = (-a[0][1] * c5 + a[0][2] * c4 - a[0][3] * c3) * inv;
    b[0][2] = (a[3][1] * s5 - a[3][2] * s4 + a[3][3] * s3) * inv;
    b[0][3] = (-a[2][1] * s5 + a[2][2] * s4 - a[2][3] * s3) * inv;
    b[1][0] = (-a[1][0] * c5 + a[1][2] * c2 - a[1][3] * c1) * inv;
    b[1][1] = (a[0][0] * c5 - a[0][2] * c2 + a[0][3] * c1) * inv;
    b[1][2] = (-a[3][0] * s5 + a[3][2] * s2 - a[3][3] * s1) * inv;
    b[1][3] = (a[2][0] * s5 - a[2][2] * s2 + a[2][3] * s1) * inv;
    b[2][0] = (a[1][0] * c4 - a[1][1] * c2 + a[1][3] * c0) * inv;
    b[2][1] = (-a[0][0] * c4 + a[0][1] * c2 - a[0][3] * c0) * inv;
    b[2][2] = (a[3][0] * s4 - a[3][1] * s2 + a[3][3] * s0) * inv;
    b[2][3] = (-a[2][0] * s4 + a[2][1] * s2 - a[2][3] * s0) * inv;
    b[3][0] = (-a[1][0] * c3 + a[1][1] * c1 - a[1][2] * c0) * inv;
    b[3][1] = (a[0][0] * c3 - a[0][1] * c1 + a[0][2] * c0) * inv;
    b[3][2] = (-a[3][0] * s3 + a[3][1] * s1 - a[3][2] * s0) * inv;
    b[3][3] = (a[2][0] * s3 - a[2][1] * s1 + a[2][2] * s0) * inv;
    return r;
  }

 private:
  std::array<std::array<double, 4>, 4> m_;
};

}  // namespace momo
