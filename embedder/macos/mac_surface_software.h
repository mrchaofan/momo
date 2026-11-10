#pragma once

// MacSoftwareSurface: 软件 surface(docs/05 §4 SoftwareBackend 的 mac 呈现端)。仅被 .mm 文件包含。
//
// raster 线程直接画进 IOSurface(零拷贝), present 后由主线程把该 IOSurface 设为视图 layer 的 contents;
// 色彩空间标在 IOSurface 上, 由 WindowServer 在 GPU 上做色彩匹配(不占主线程)。
//
// 背压(关键): 主线程最多只挂起一帧——raster 每次 present 只覆盖"待显示"槽, 只有槽从空变满时才
// 向主队列投递一次。raster 比主线程快时丢弃中间帧, 主队列与内存都有上界, 主线程始终能处理输入事件。
// 缓冲: 最多 3 个 IOSurface(显示中 / 待显示 / 绘制中);全忙时本帧丢弃(AcquireFrame 返回 Unavailable)。
//
// 线程: AcquireFrame/present 在 raster 线程;构造/析构在 platform 线程(由窗口或 Metal surface 拥有)。
// 生命周期: 主线程块只持有共享的 PresentState(其中弱引用视图), 窗口先销毁也安全。

#include <IOSurface/IOSurfaceRef.h>

#include <memory>
#include <vector>

#include "embedder/common/engine_platform.h"
#include "engine/platform/render_surface.h"

#ifdef __OBJC__
@class NSView;
#endif

namespace momo::embedder::macos {

class MacSoftwareSurface final : public platform::RenderSurface {
 public:
  explicit MacSoftwareSurface(NSView* view);
  ~MacSoftwareSurface() override;

  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size physical_size) override;

 private:
  struct PresentState;

  IOSurfaceRef PickBuffer(int width, int height);
  bool Present(IOSurfaceRef surface);

  std::shared_ptr<PresentState> state_;
  std::vector<IOSurfaceRef> buffers_;  // 仅 raster 线程访问(各持有一个引用)
};

}  // namespace momo::embedder::macos
