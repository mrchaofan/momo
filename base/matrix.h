#pragma once

// Matrix: 4x4 仿射/透视变换矩阵(TransformLayer、hit test 用, ARCHITECTURE.md §4.5)。
// header-only, 属于 momo_base。值类型, 线程无关。
//
// 约定: 行主序存储 m[row][col], 作用于列向量 p' = M * p。
// 组合: (A * B) 表示先应用 B 再应用 A(与 Skia SkM44 一致)。

#include <array>
#include <cmath>

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

  // 先应用 other 再应用 this。
  Matrix& PreConcat(const Matrix& other) { return *this = *this * other; }

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

 private:
  std::array<std::array<double, 4>, 4> m_;
};

}  // namespace momo
