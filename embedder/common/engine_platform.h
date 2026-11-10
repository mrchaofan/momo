#pragma once

// embedder 内统一以 `platform::` 指代 engine 的平台契约(engine/platform/: RenderSurface、VsyncSource、
// 输入事件、IME、PlatformServices、ViewportMetrics…, 命名空间 momo::engine::platform)。
// embedder 自己的平台抽象(PlatformIntegration、PlatformWindow、WindowDelegate、WindowSpec、DisplayInfo)
// 直接位于 momo::embedder, 不带前缀。

namespace momo::engine::platform {}

namespace momo::embedder {
namespace platform = ::momo::engine::platform;
}  // namespace momo::embedder
