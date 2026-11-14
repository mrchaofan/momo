#include "embedder/windows/win_frame.h"

namespace momo::embedder::windows {

namespace {

bool Contains(const Rect& r, Point p) { return p.x >= r.x && p.x < r.right() && p.y >= r.y && p.y < r.bottom(); }

}  // namespace

WinCaptionButtonsLayout LayoutCaptionButtons(double client_width, double height, bool maximize_enabled) {
  WinCaptionButtonsLayout layout;
  if (client_width <= 0 || height <= 0) {
    return layout;
  }
  const double w = kCaptionButtonWidth;
  const double right = client_width;
  layout.close = {right - w, 0, w, height};
  layout.maximize = {right - 2 * w, 0, w, height};
  layout.minimize = {right - 3 * w, 0, w, height};
  layout.bounds = {right - 3 * w, 0, 3 * w, height};
  layout.maximize_enabled = maximize_enabled;
  return layout;
}

WinFrameHit HitTestFrame(const WinFrameState& state, Point p) {
  const double w = state.client.width;
  const double h = state.client.height;
  if (p.x < 0 || p.y < 0 || p.x >= w || p.y >= h) {
    return WinFrameHit::kClient;  // 不在内容区内: 由系统按标准框架处理
  }
  if (state.resize.resizable && !state.maximized && !state.fullscreen) {
    const WindowResizeOptions& o = state.resize;
    const bool left = p.x < o.border_left;
    const bool right = w - p.x <= o.border_right;
    const bool top = state.title_bar_hidden && p.y < o.border_top;
    const bool bottom = h - p.y <= o.border_bottom;
    if (top && left) return WinFrameHit::kTopLeft;
    if (top && right) return WinFrameHit::kTopRight;
    if (bottom && left) return WinFrameHit::kBottomLeft;
    if (bottom && right) return WinFrameHit::kBottomRight;
    if (left) return WinFrameHit::kLeft;
    if (right) return WinFrameHit::kRight;
    if (top) return WinFrameHit::kTop;
    if (bottom) return WinFrameHit::kBottom;
  }
  if (state.title_bar_hidden && !state.fullscreen && !state.buttons.empty()) {
    const WinCaptionButtonsLayout& b = state.buttons;
    if (Contains(b.close, p)) return WinFrameHit::kCloseButton;
    if (Contains(b.maximize, p)) {
      return b.maximize_enabled ? WinFrameHit::kMaximizeButton : WinFrameHit::kDisabledButton;
    }
    if (Contains(b.minimize, p)) return WinFrameHit::kMinimizeButton;
  }
  return WinFrameHit::kClient;
}

std::optional<Rect> WindowControlsRect(const WinFrameState& state) {
  if (!state.title_bar_hidden || state.fullscreen || state.buttons.empty()) {
    return std::nullopt;
  }
  return state.buttons.bounds;
}

}  // namespace momo::embedder::windows
