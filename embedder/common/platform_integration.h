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
  // delegate 必须比返回的窗口活得久。窗口创建后不显示(options.show 由调用方执行)。
  virtual StatusOr<std::unique_ptr<PlatformWindow>> CreateWindow(const WindowOptions& options,
                                                                 WindowDelegate* delegate) = 0;
  // 应用的帧时钟来源: 整个应用只创建一个(所有窗口共用一个 Animator), 与窗口无关。返回的
  // platform::VsyncSource 归属 platform 线程(momo::Object), 释放即 DeleteLater。
  virtual StatusOr<ObjectPtr<platform::VsyncSource>> CreateVsyncSource() = 0;
  // 帧时钟跟随 window 所在的显示器(窗口跨屏时自动切换);nullptr = 主显示器。作用于 CreateVsyncSource
  // 返回的那个来源(已释放或尚未创建时什么也不做)。来源不依赖窗口存活。节拍与显示器无关的平台
  // (Windows: DWM 的合成节奏)不必实现。
  virtual void SetVsyncWindow(const PlatformWindow* window) { (void)window; }
  virtual PlatformServices& services() = 0;
  // 应用级能力(隐藏/显示/激活)。
  virtual PlatformApp& app() = 0;
  virtual std::vector<DisplayInfo> displays() const = 0;
  // OS 主线程的原生消息循环。embedder 用 NativeMessagePump 在其上运行 platform EventLoop。
  virtual NativeLoop& native_loop() = 0;
};

}  // namespace momo::embedder
