// MetalRasterizer 单测: 渲染到离屏 MTLTexture 并回读, 与软件光栅结果比对。
// 无可用 Metal 设备(如部分 CI 虚拟机)时跳过。

#include "engine/graphics/gpu/metal_rasterizer.h"

#import <Metal/Metal.h>

#include <cstdlib>
#include <memory>
#include <vector>

#include "engine/graphics/display_list.h"
#include "engine/graphics/layer.h"
#include "engine/graphics/layer_tree.h"
#include "engine/graphics/software_rasterizer.h"
#include "gtest/gtest.h"

namespace momo::engine::graphics {
namespace {

Paint Fill(uint32_t color) {
  Paint p;
  p.color = color;
  return p;
}

LayerTree MakeScene(int size) {
  DisplayListBuilder builder;
  builder.DrawRect({0, 0, 20, 20}, Fill(0xFFFF0000));
  builder.DrawRect({10, 10, 20, 20}, Fill(0x8000FF00));
  std::shared_ptr<const DisplayList> picture = builder.Build();
  auto root = std::make_shared<ContainerLayer>();
  auto offset = std::make_shared<OffsetLayer>(Point{8, 4});
  offset->Add(std::make_shared<PictureLayer>(Point{0, 0}, picture));
  root->Add(offset);
  return LayerTree({static_cast<double>(size), static_cast<double>(size)}, 1.0, root);
}

int ChannelDiff(uint32_t a, uint32_t b) {
  int worst = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const int d = std::abs(static_cast<int>((a >> shift) & 0xFF) -
                           static_cast<int>((b >> shift) & 0xFF));
    worst = d > worst ? d : worst;
  }
  return worst;
}

TEST(MetalRasterizerTest, RejectsMissingDevice) {
  EXPECT_FALSE(MetalRasterizer::Create(nullptr, nullptr).ok());
}

TEST(MetalRasterizerTest, MatchesSoftwareRasterizer) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      GTEST_SKIP() << "No Metal device";
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    auto rasterizer = MetalRasterizer::Create((__bridge void*)device, (__bridge void*)queue);
    ASSERT_TRUE(rasterizer.ok()) << rasterizer.status();

    constexpr int kSize = 48;
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:kSize
                                                          height:kSize
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeManaged;
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    ASSERT_NE(texture, nil);

    LayerTree tree = MakeScene(kSize);
    ASSERT_TRUE((*rasterizer)
                    ->DrawToTexture(tree, (__bridge void*)texture, tree.frame_size(), 0xFFFFFFFF)
                    .ok());
    // Managed 纹理: 回读前同步到 CPU。
    id<MTLCommandBuffer> blit_buffer = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [blit_buffer blitCommandEncoder];
    [blit synchronizeResource:texture];
    [blit endEncoding];
    [blit_buffer commit];
    [blit_buffer waitUntilCompleted];

    std::vector<uint32_t> gpu(static_cast<size_t>(kSize) * kSize);
    [texture getBytes:gpu.data()
          bytesPerRow:kSize * 4
           fromRegion:MTLRegionMake2D(0, 0, kSize, kSize)
          mipmapLevel:0];

    OwnedPixelBuffer cpu(kSize, kSize);
    LayerTree cpu_tree = MakeScene(kSize);
    ASSERT_TRUE(RasterizeLayerTree(cpu_tree, cpu.view(), 0xFFFFFFFF).ok());

    int worst = 0;
    for (size_t i = 0; i < gpu.size(); ++i) {
      const int d = ChannelDiff(gpu[i], cpu.data()[i]);
      worst = d > worst ? d : worst;
    }
    EXPECT_LE(worst, 2);
    // 抽查: 红块内部与白底。
    EXPECT_EQ(gpu[static_cast<size_t>(6) * kSize + 10], 0xFFFF0000u);
    EXPECT_EQ(gpu[static_cast<size_t>(46) * kSize + 46], 0xFFFFFFFFu);
  }
}

}  // namespace
}  // namespace momo::engine::graphics
