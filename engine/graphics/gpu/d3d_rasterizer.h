#pragma once

// D3DRasterizer: Ganesh(Skia GPU)Direct3D 12 后端的 graphics 侧实现(docs/05 §4 Direct3DBackend;对应 macOS 的
// MetalRasterizer)。公共头不含 Skia/D3D 类型(docs/10 §1): D3D 对象以 void* 传入(ID3D12Device* /
// ID3D12CommandQueue* / ID3D12Resource*, 非拥有)。
//
// 绘制目标是 swapchain 的后台缓冲(状态 D3D12_RESOURCE_STATE_PRESENT): 每帧包装为 render target, 画完后由 Skia
// 转回 PRESENT 状态并提交, 且**等待 GPU 完成**(GrSyncCpu::kYes)。等待让 Skia 在返回前释放对后台缓冲的全部引用,
// surface 才能随时在 raster 线程 ResizeBuffers(DXGI 要求届时没有任何对后台缓冲的引用);代价是 CPU 与 GPU 不
// 重叠, 对以 2D 界面为主的帧可以接受。
//
// 线程约定: 只在 raster 线程创建、使用与析构(持有的 GrDirectContext 归 raster 线程, docs/08 §3)。
// 析构前同步等待已提交的 GPU 工作完成, 析构返回后不再触碰传入的 device/queue。
// 仅 Windows 编译(engine/CMakeLists.txt)。

#include <cstdint>
#include <memory>

#include "base/geometry.h"
#include "base/macros.h"
#include "base/status.h"
#include "engine/graphics/composition.h"
#include "engine/graphics/layer_tree.h"

namespace momo::engine::graphics {

class D3DRasterizer {
 public:
  // device: ID3D12Device*;queue: ID3D12CommandQueue*(DIRECT, 须属于该 device)。内部各自 AddRef。
  static StatusOr<std::unique_ptr<D3DRasterizer>> Create(void* device, void* queue);
  ~D3DRasterizer();

  // 把整棵树画进 resource(ID3D12Resource*, BGRA8/RGBA8 后台缓冲, 状态 PRESENT)并等待完成(不 present)。
  // clear_color: 0xAARRGGBB(默认全透明, 与软件后端一致)。
  Status Draw(LayerTree& tree, void* resource, Size size, uint32_t clear_color = 0x00000000);
  // 平台视图合成的一片(docs/05 §9): resource 像素 (0,0) 对应设备坐标 origin。
  Status DrawSlice(const Composition& composition, size_t slice, void* resource, Point origin,
                   uint32_t clear_color = 0x00000000);
  // 阻塞等待已提交的 GPU 工作完成(关闭前、测试回读使用)。
  void WaitForGpu();

 private:
  struct Impl;
  explicit D3DRasterizer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(D3DRasterizer);
};

}  // namespace momo::engine::graphics
