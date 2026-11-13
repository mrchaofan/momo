#pragma once

// 平台视图(platform view)的合成布局契约(docs/05 §9)。engine 产出, embedder 消费。
//
// 平台视图 = 由扩展创建的系统原生视图(NSView / HWND, 如 WKWebView), 由 embedder 挂进窗口的视图层级,
// 与 momo 的画面交替叠放。raster 线程按 LayerTree 中 PlatformViewLayer 的位置把画面切成若干片(slice):
//
//   z 序(下 → 上):  base 主帧 │ views[0] │ overlays[0] │ views[1] │ overlays[1] │ …
//
// overlays[i] 是绘制顺序上位于 views[i] 之后、views[i+1] 之前的全部 momo 内容, 画在独立的透明图层上。
// 全部坐标为物理像素, 根坐标系 = 窗口内容区, 左上原点, y 向下。
// 线程约定: 纯数据(POD + vector), 在 raster 线程生成, 可拷贝到 platform 线程使用。

#include <cstdint>
#include <optional>
#include <vector>

#include "base/geometry.h"

namespace momo::engine::platform {

// 祖先裁剪(设备空间轴对齐矩形 + 圆角半径;变换含旋转/斜切时为包围盒近似)。
struct PlatformViewClip {
  Rect rect;
  double radius = 0;
};

struct PlatformViewGeometry {
  int64_t view_id = 0;
  // 视图在设备空间的位置与大小(本地矩形经变换后的轴对齐包围盒)。
  Rect frame;
  // 变换只含平移与正向缩放时为 true;否则 frame 只是近似(embedder 不做旋转/斜切)。
  bool axis_aligned = true;
  // 从外到内的祖先裁剪。可见区域 = frame ∩ 所有 clips。
  std::vector<PlatformViewClip> clips;
  // 祖先 OpacityLayer 的累计透明度(0..1)。
  double opacity = 1.0;
  // 本视图之上(z 序更高的 overlay 中)与之相交的 momo 内容范围(已与 frame 求交)。
  // 落在其中的指针事件应交给 momo, 而不是平台视图。
  std::vector<Rect> occluders;
};

struct CompositionLayout {
  // 按 z 序从下到上。
  std::vector<PlatformViewGeometry> views;
  // 与 views 等长:overlays[i] 位于 views[i] 之上;nullopt = 该片没有内容(不需要图层)。
  // 矩形已取整为整数像素并与帧范围求交。
  std::vector<std::optional<Rect>> overlays;
};

}  // namespace momo::engine::platform
