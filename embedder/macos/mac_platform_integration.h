#pragma once

// macOS 平台集成工厂。纯 C++ 头, 供 embedder/macos 使用。
// 必须在主线程调用;内部会初始化 NSApplication(sharedApplication)。

#include <memory>

#include "embedder/common/platform_integration.h"

namespace momo::embedder::macos {

std::unique_ptr<PlatformIntegration> CreateMacPlatformIntegration();

}  // namespace momo::embedder::macos
