#pragma once

// PlatformIntegration: embedder 平台抽象的工厂与根入口(docs/03 §2)。engine 不可见;具体实现由平台入口创建并注入 Application。
// 线程约定: 仅 platform 线程调用(native_loop() 返回的对象按 base/native_loop.h 约定线程安全)。

#include <memory>
#include <vector>

#include "base/native_loop.h"
#include "base/status.h"
#include "embedder/common/display.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_app.h"
#include "embedder/common/platform_window.h"
#include "embedder/common/platform_services.h"
#include "engine/platform/vsync_source.h"

namespace momo::embedder {

class PlatformIntegration {
 public:
  virtual ~PlatformIntegration() = default;
  // delegate 必须比返回的窗口活得久。
  virtual StatusOr<std::unique_ptr<PlatformWindow>> CreateWindow(const WindowSpec& spec,
                                                                 WindowDelegate* delegate) = 0;
  // 返回的 platform::VsyncSource 归属 platform 线程(momo::Object), 释放即 DeleteLater。
  virtual StatusOr<ObjectPtr<platform::VsyncSource>> CreateVsyncSource(const PlatformWindow& window) = 0;
  virtual PlatformServices& services() = 0;
  // 应用级能力(隐藏/显示/激活)。
  virtual PlatformApp& app() = 0;
  virtual std::vector<DisplayInfo> displays() const = 0;
  // OS 主线程的原生消息循环。embedder 用 NativeMessagePump 在其上运行 platform EventLoop。
  virtual NativeLoop& native_loop() = 0;
};

}  // namespace momo::embedder
