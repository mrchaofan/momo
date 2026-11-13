#pragma once

// embedder 内统一以 `platform::` 指代 engine 的平台契约(engine/platform/: RenderSurface、VsyncSource、
// 输入事件、ViewportMetrics、ViewId…, 命名空间 momo::engine::platform)——引擎对宿主的全部要求。
// embedder 自己的平台抽象(PlatformIntegration、PlatformWindow、WindowDelegate、WindowSpec、DisplayInfo、
// PlatformServices、TextInputConnection、SystemCursor 等窗口类型)直接位于 momo::embedder, 不带前缀:
// 引擎看不到它们, 它们只经宿主注册的通道与 JS 交互。

namespace momo::engine::platform {}

namespace momo::embedder {
namespace platform = ::momo::engine::platform;
}  // namespace momo::embedder
