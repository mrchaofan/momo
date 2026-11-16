// MacMetalSurface: 设备共享(全部窗口共用应用级的 MTLDevice / MTLCommandQueue)与两种呈现方式。
// 呈现队列与拖拽缩放同步本身的行为见 embedder/common/surface_unittests.cc;真实的拖拽缩放无法在测试里驱动,
// 这里由测试线程扮演 raster 线程取帧、呈现。

#import <Cocoa/Cocoa.h>

#include "embedder/macos/mac_surface_metal.h"
#include "gtest/gtest.h"

namespace momo::embedder::macos {
namespace {

class MacSurfaceMetalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto device = MacMetalDevice::Create();
    if (!device.ok()) {
      GTEST_SKIP() << "No Metal device: " << device.status();
    }
    device_ = std::move(device).value();
    presents_ = PresentQueue::Create([this](std::shared_ptr<PresentQueue>) { wakes_++; });
    view_ = MakeView();
    surface_ = std::make_unique<MacMetalSurface>(view_, device_, presents_);
  }

  static NSView* MakeView() {
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 80)];
    view.wantsLayer = YES;
    return view;
  }

  std::shared_ptr<const MacMetalDevice> device_;
  std::shared_ptr<PresentQueue> presents_;
  int wakes_ = 0;
  NSView* view_ = nil;
  std::unique_ptr<MacMetalSurface> surface_;
};

// 两个窗口的 surface 报告同一设备与队列: 引擎据此共用一个 GPU 后端。
TEST_F(MacSurfaceMetalTest, SurfacesShareTheApplicationDevice) {
  NSView* other_view = MakeView();
  MacMetalSurface other(other_view, device_, PresentQueue::Create(nullptr));
  const platform::GpuContextInfo a = surface_->gpu_context();
  const platform::GpuContextInfo b = other.gpu_context();
  EXPECT_NE(a.device, nullptr);
  EXPECT_NE(a.queue, nullptr);
  EXPECT_EQ(a.device, b.device);
  EXPECT_EQ(a.queue, b.queue);
}

// 平常: 帧在取帧的线程上呈现, 不经过主线程的队列。
TEST_F(MacSurfaceMetalTest, PresentsOnTheRasterThreadByDefault) {
  auto frame = surface_->AcquireFrame({200, 160});
  ASSERT_TRUE(frame.ok()) << frame.status();
  EXPECT_FALSE(presents_->has_presented());
  ASSERT_TRUE(frame->present());
  EXPECT_TRUE(presents_->has_presented());
  EXPECT_EQ(presents_->pending(), 0);
  EXPECT_EQ(wakes_, 0);
}

// 拖拽缩放期间: 帧入队, 由主线程呈现(与窗口尺寸同一事务)。
TEST_F(MacSurfaceMetalTest, PresentsThroughTheQueueWhileLiveResizing) {
  surface_->SetLiveResizing(true);
  auto frame = surface_->AcquireFrame({200, 160});
  ASSERT_TRUE(frame.ok()) << frame.status();
  ASSERT_TRUE(frame->present());
  EXPECT_EQ(presents_->pending(), 1);
  EXPECT_EQ(wakes_, 1);
  EXPECT_FALSE(presents_->has_presented());
  EXPECT_TRUE(presents_->WaitForFrame({200, 160}, std::chrono::seconds(2), [] { return true; }));
  EXPECT_EQ(presents_->pending(), 0);
}

}  // namespace
}  // namespace momo::embedder::macos
