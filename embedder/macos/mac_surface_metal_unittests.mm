// 拖拽缩放同步(mac_surface_metal.h WaitForFrame): 主线程等待新尺寸的帧并在当前事务里呈现;等待期间执行
// 平台任务(pump);超时返回 false。真实的拖拽缩放无法在测试里驱动, 这里用后台线程扮演 raster 线程。

#import <Cocoa/Cocoa.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "embedder/macos/mac_surface_metal.h"
#include "gtest/gtest.h"

namespace momo::embedder::macos {
namespace {

using namespace std::chrono_literals;

class MacSurfaceMetalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    view_ = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 80)];
    view_.wantsLayer = YES;
    auto surface = MacMetalSurface::Create(view_);
    if (!surface.ok()) {
      GTEST_SKIP() << "No Metal device: " << surface.status();
    }
    surface_ = std::move(surface).value();
    surface_->SetLiveResizing(true);
  }

  // 扮演 raster 线程: delay 后取一帧 size 并呈现。
  std::thread PresentLater(Size size, std::chrono::milliseconds delay) {
    return std::thread([this, size, delay] {
      std::this_thread::sleep_for(delay);
      auto frame = surface_->AcquireFrame(size);
      ASSERT_TRUE(frame.ok()) << frame.status();
      ASSERT_TRUE(frame->present());
    });
  }

  NSView* view_ = nil;
  std::unique_ptr<MacMetalSurface> surface_;
};

// 新尺寸的帧在等待期间到达: 主线程在 WaitForFrame 内呈现它(不必回到 GCD 主队列)。
TEST_F(MacSurfaceMetalTest, WaitsForTheFrameOfTheNewSize) {
  int pumps = 0;
  std::thread raster = PresentLater({200, 160}, 30ms);
  const bool got = surface_->WaitForFrame({200, 160}, 2s, [&] { ++pumps; });
  raster.join();
  EXPECT_TRUE(got);
  EXPECT_GT(pumps, 0) << "platform tasks keep running while waiting (vsync goes through them)";
  EXPECT_TRUE(surface_->has_presented());
}

// 先到的旧尺寸帧也按序呈现, 但不结束等待;等到新尺寸的帧才返回。
TEST_F(MacSurfaceMetalTest, OlderFramesArePresentedInOrderWithoutEndingTheWait) {
  std::thread old_frame = PresentLater({100, 80}, 0ms);
  old_frame.join();
  std::thread raster = PresentLater({240, 180}, 30ms);
  EXPECT_TRUE(surface_->WaitForFrame({240, 180}, 2s, [] {}));
  raster.join();
}

// 应用没有及时出帧: 超时返回 false(内容暂时拉伸), 不卡住窗口拖动。
TEST_F(MacSurfaceMetalTest, TimesOutWithoutAFrame) {
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(surface_->WaitForFrame({300, 200}, 50ms, [] {}));
  const auto waited = std::chrono::steady_clock::now() - start;
  EXPECT_GE(waited, 50ms);
  EXPECT_LT(waited, 1s);
}

}  // namespace
}  // namespace momo::embedder::macos
