#pragma once

// 合成布局(engine/platform/platform_view.h)上的几何计算: 两个平台的平台视图宿主(macOS MomoPlatformViews、
// Windows WinPlatformViews)共用, 不依赖任何平台 API。坐标为物理像素, 窗口内容区左上原点。

#include <cstdint>
#include <functional>
#include <optional>

#include "base/geometry.h"
#include "engine/platform/platform_view.h"

namespace momo::embedder {

// 视图的可见区域 = frame ∩ 全部祖先裁剪;完全不可见时为 nullopt。
std::optional<Rect> PlatformViewVisibleRect(const engine::platform::PlatformViewGeometry& view);

// 作用在可见区域上的圆角: 取与可见区域重合(误差 1px 内)的祖先圆角裁剪中最大的半径。多层不同圆角或圆角裁剪
// 不贴合可见区域时只能近似(宿主的视图容器只有一个圆角)。
double PlatformViewCornerRadius(const engine::platform::PlatformViewGeometry& view, const Rect& visible);

// point 处最上层接收指针的平台视图: 在它的可见区域内、且之上没有 momo 内容(occluders);没有时为 0(归 momo)。
// hosted(view_id) 为 false 的视图跳过(例如已被别的窗口接管)。
int64_t PlatformViewAt(const engine::platform::CompositionLayout& layout, Point point,
                       const std::function<bool(int64_t)>& hosted);

}  // namespace momo::embedder
