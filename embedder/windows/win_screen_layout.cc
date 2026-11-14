#include "embedder/windows/win_screen_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace momo::embedder::windows {

namespace {

// 两个区间 [a0, a1) 与 [b0, b1) 的重叠长度。
double Overlap(double a0, double a1, double b0, double b1) {
  return std::min(a1, b1) - std::max(a0, b0);
}

// 把 m 紧贴已放置的 p 放置。返回是否相邻(共享一条边且边上有重叠)。
bool PlaceAdjacent(const WinScreenLayout::Monitor& p, WinScreenLayout::Monitor& m) {
  const Rect& pp = p.info.physical;
  const Rect& mp = m.info.physical;
  const double w = m.logical.width;
  const double h = m.logical.height;
  const bool vertical_overlap = Overlap(pp.y, pp.bottom(), mp.y, mp.bottom()) > 0;
  const bool horizontal_overlap = Overlap(pp.x, pp.right(), mp.x, mp.right()) > 0;
  // 沿共享边的偏移按 p 的 DPR 换算(p 的逻辑坐标系里, m 的边从这里开始)。
  const double along_y = p.logical.y + (mp.y - pp.y) / p.info.dpr;
  const double along_x = p.logical.x + (mp.x - pp.x) / p.info.dpr;
  if (vertical_overlap && mp.x == pp.right()) {
    m.logical = {p.logical.right(), along_y, w, h};
    return true;
  }
  if (vertical_overlap && mp.right() == pp.x) {
    m.logical = {p.logical.x - w, along_y, w, h};
    return true;
  }
  if (horizontal_overlap && mp.y == pp.bottom()) {
    m.logical = {along_x, p.logical.bottom(), w, h};
    return true;
  }
  if (horizontal_overlap && mp.bottom() == pp.y) {
    m.logical = {along_x, p.logical.y - h, w, h};
    return true;
  }
  return false;
}

// 点到矩形的距离平方(在矩形内为 0)。
double DistanceSquared(const Rect& r, Point p) {
  const double dx = p.x < r.x ? r.x - p.x : (p.x >= r.right() ? p.x - r.right() : 0);
  const double dy = p.y < r.y ? r.y - p.y : (p.y >= r.bottom() ? p.y - r.bottom() : 0);
  return dx * dx + dy * dy;
}

bool Contains(const Rect& r, Point p) { return p.x >= r.x && p.x < r.right() && p.y >= r.y && p.y < r.bottom(); }

template <typename RectOf>
const WinScreenLayout::Monitor* Nearest(const std::vector<WinScreenLayout::Monitor>& monitors, Point p,
                                        RectOf rect_of) {
  const WinScreenLayout::Monitor* best = nullptr;
  double best_distance = std::numeric_limits<double>::infinity();
  for (const WinScreenLayout::Monitor& m : monitors) {
    const Rect& r = rect_of(m);
    if (Contains(r, p)) {
      return &m;
    }
    const double d = DistanceSquared(r, p);
    if (d < best_distance) {
      best_distance = d;
      best = &m;
    }
  }
  return best;
}

}  // namespace

WinScreenLayout::WinScreenLayout(std::vector<WinMonitorInfo> monitors) {
  // 主屏在前(只取第一个标记为主屏的);没有主屏标记时第一个即主屏。
  std::stable_partition(monitors.begin(), monitors.end(), [](const WinMonitorInfo& m) { return m.primary; });
  for (WinMonitorInfo& info : monitors) {
    if (info.dpr <= 0) info.dpr = 1.0;
    Monitor m;
    m.info = info;
    m.logical = {0, 0, info.physical.width / info.dpr, info.physical.height / info.dpr};
    monitors_.push_back(m);
  }
  if (monitors_.empty()) {
    return;
  }
  const double primary_dpr = monitors_[0].info.dpr;
  monitors_[0].logical.x = monitors_[0].info.physical.x / primary_dpr;
  monitors_[0].logical.y = monitors_[0].info.physical.y / primary_dpr;
  std::vector<bool> placed(monitors_.size(), false);
  placed[0] = true;
  // 从主屏开始沿相邻关系逐个放置(每轮至少放置一个, 否则剩下的都不与已放置的相邻)。
  for (bool progress = true; progress;) {
    progress = false;
    for (size_t i = 0; i < monitors_.size(); ++i) {
      if (placed[i]) continue;
      for (size_t j = 0; j < monitors_.size(); ++j) {
        if (placed[j] && PlaceAdjacent(monitors_[j], monitors_[i])) {
          placed[i] = true;
          progress = true;
          break;
        }
      }
    }
  }
  for (size_t i = 0; i < monitors_.size(); ++i) {
    if (!placed[i]) {
      monitors_[i].logical.x = monitors_[i].info.physical.x / primary_dpr;
      monitors_[i].logical.y = monitors_[i].info.physical.y / primary_dpr;
    }
  }
}

const WinScreenLayout::Monitor* WinScreenLayout::MonitorForPhysical(Point physical) const {
  return Nearest(monitors_, physical, [](const Monitor& m) -> const Rect& { return m.info.physical; });
}

const WinScreenLayout::Monitor* WinScreenLayout::MonitorForLogical(Point logical) const {
  return Nearest(monitors_, logical, [](const Monitor& m) -> const Rect& { return m.logical; });
}

Point WinScreenLayout::ToLogical(Point physical) const {
  const Monitor* m = MonitorForPhysical(physical);
  if (m == nullptr) {
    return physical;
  }
  return {m->logical.x + (physical.x - m->info.physical.x) / m->info.dpr,
          m->logical.y + (physical.y - m->info.physical.y) / m->info.dpr};
}

Point WinScreenLayout::ToPhysical(Point logical) const {
  const Monitor* m = MonitorForLogical(logical);
  if (m == nullptr) {
    return logical;
  }
  return {m->info.physical.x + (logical.x - m->logical.x) * m->info.dpr,
          m->info.physical.y + (logical.y - m->logical.y) * m->info.dpr};
}

Rect WinScreenLayout::ToLogical(const Rect& physical, double dpr) const {
  if (dpr <= 0) dpr = 1.0;
  const Point origin = ToLogical(Point{physical.x, physical.y});
  return {origin.x, origin.y, physical.width / dpr, physical.height / dpr};
}

Rect WinScreenLayout::LogicalWorkArea(const Monitor& monitor) const {
  const Rect& work = monitor.info.physical_work;
  const double dpr = monitor.info.dpr;
  return {monitor.logical.x + (work.x - monitor.info.physical.x) / dpr,
          monitor.logical.y + (work.y - monitor.info.physical.y) / dpr, work.width / dpr, work.height / dpr};
}

}  // namespace momo::embedder::windows
