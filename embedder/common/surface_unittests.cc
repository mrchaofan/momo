// PresentQueue(呈现队列与拖拽缩放同步)与 FallbackSurface(GPU → 软件回退)。两者与平台无关, 后台线程扮演
// raster 线程。

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "embedder/common/fallback_surface.h"
#include "embedder/common/present_queue.h"
#include "gtest/gtest.h"

namespace momo::embedder {
namespace {

using namespace std::chrono_literals;
using engine::platform::SurfaceFrame;
using engine::platform::SurfaceKind;

// ---- PresentQueue ----

class PresentQueueTest : public ::testing::Test {
 protected:
  std::shared_ptr<PresentQueue> queue_ =
      PresentQueue::Create([this](std::shared_ptr<PresentQueue>) { wakes_++; });
  std::atomic<int> wakes_{0};
  std::vector<int> presented_;  // platform 线程

  // 扮演 raster 线程: delay 后入队一帧。
  std::thread PostLater(Size size, int id, std::chrono::milliseconds delay) {
    return std::thread([this, size, id, delay] {
      std::this_thread::sleep_for(delay);
      queue_->Post(size, [this, id] { presented_.push_back(id); });
    });
  }
};

TEST_F(PresentQueueTest, DrainRunsPostedFramesInOrder) {
  EXPECT_FALSE(queue_->has_presented());
  queue_->Post({10, 10}, [this] { presented_.push_back(1); });
  queue_->Post({20, 20}, [this] { presented_.push_back(2); });
  EXPECT_EQ(queue_->pending(), 2);
  EXPECT_TRUE(presented_.empty()) << "presents run on the platform thread, not where they are posted";
  queue_->Drain();
  EXPECT_EQ(presented_, (std::vector<int>{1, 2}));
  EXPECT_EQ(queue_->pending(), 0);
  EXPECT_TRUE(queue_->has_presented());
}

// raster 比 platform 线程快时唤醒不堆积: 一次唤醒在途期间不再唤醒;Drain 之后的帧重新唤醒。
TEST_F(PresentQueueTest, WakesOncePerDrain) {
  queue_->Post({10, 10}, nullptr);
  queue_->Post({10, 10}, nullptr);
  queue_->Post({10, 10}, nullptr);
  EXPECT_EQ(wakes_.load(), 1);
  queue_->Drain();
  queue_->Post({10, 10}, nullptr);
  EXPECT_EQ(wakes_.load(), 2);
}

// 新尺寸的帧在等待期间到达: platform 线程在 WaitForFrame 内呈现它(不必回到事件循环)。
TEST_F(PresentQueueTest, WaitsForTheFrameOfTheNewSize) {
  int pumps = 0;
  std::thread raster = PostLater({200, 160}, 1, 30ms);
  const bool got = queue_->WaitForFrame({200, 160}, 2s, [&] {
    ++pumps;
    return true;
  });
  raster.join();
  EXPECT_TRUE(got);
  EXPECT_GT(pumps, 0) << "platform tasks keep running while waiting (vsync goes through them)";
  EXPECT_EQ(presented_, (std::vector<int>{1}));
}

// 先到的旧尺寸帧也按序呈现, 但不结束等待。
TEST_F(PresentQueueTest, OlderFramesArePresentedInOrderWithoutEndingTheWait) {
  queue_->Post({100, 80}, [this] { presented_.push_back(1); });
  std::thread raster = PostLater({240, 180}, 2, 30ms);
  EXPECT_TRUE(queue_->WaitForFrame({240, 180}, 2s, [] { return true; }));
  raster.join();
  EXPECT_EQ(presented_, (std::vector<int>{1, 2}));
}

// 在 raster 线程呈现的帧(不入队)同样结束等待。
TEST_F(PresentQueueTest, FramesPresentedOnTheRasterThreadEndTheWait) {
  std::thread raster([this] {
    std::this_thread::sleep_for(30ms);
    queue_->NotePresented({300, 200});
  });
  EXPECT_TRUE(queue_->WaitForFrame({300, 200}, 2s, [] { return true; }));
  raster.join();
}

// 应用没有及时出帧: 超时返回 false(内容暂时拉伸), 不卡住窗口拖动。
TEST_F(PresentQueueTest, TimesOutWithoutAFrame) {
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(queue_->WaitForFrame({300, 200}, 50ms, [] { return true; }));
  const auto waited = std::chrono::steady_clock::now() - start;
  EXPECT_GE(waited, 50ms);
  EXPECT_LT(waited, 1s);
}

// 拖拽缩放同步: 先把新尺寸交给引擎, 再等到它的帧。
TEST_F(PresentQueueTest, SynchronizeResizeNotifiesThenWaitsForTheFrame) {
  // 还没出过帧: 等待只会超时, 所以只通知。
  int notified = 0;
  const auto start = std::chrono::steady_clock::now();
  queue_->SynchronizeResize({200, 160}, [&] { ++notified; });
  EXPECT_EQ(notified, 1);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 50ms) << "no wait before the first frame";

  queue_->NotePresented({100, 80});
  std::thread raster;
  queue_->SynchronizeResize({200, 160}, [&] {
    ++notified;
    raster = PostLater({200, 160}, 7, 20ms);  // 引擎收到新尺寸后画出这一帧
    // 等待期间执行的任务又改变了窗口尺寸: 嵌套的调用只通知, 由外层的等待覆盖。
    queue_->SynchronizeResize({210, 160}, [&] { ++notified; });
  });
  raster.join();
  EXPECT_EQ(notified, 3);
  EXPECT_EQ(presented_, (std::vector<int>{7})) << "the frame was presented inside the call";
}

// 等待期间窗口被销毁(pump 返回 false): 立即返回;队列由等待方自己持有, 不悬空。
TEST_F(PresentQueueTest, StopsWaitingWhenTheOwnerIsDestroyedInThePump) {
  PresentQueue* raw = queue_.get();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(raw->WaitForFrame({300, 200}, 5s, [this] {
    queue_.reset();  // 窗口放手
    return false;
  }));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}

// ---- FallbackSurface ----

class FakeSoftwareSurface final : public engine::platform::RenderSurface {
 public:
  SurfaceKind kind() const override { return SurfaceKind::kSoftware; }
  StatusOr<SurfaceFrame> AcquireFrame(Size size) override {
    acquired++;
    SurfaceFrame frame;
    frame.kind = SurfaceKind::kSoftware;
    frame.size = size;
    return frame;
  }
  int acquired = 0;
};

class FakeGpuSurface final : public GpuSurface {
 public:
  SurfaceKind kind() const override { return SurfaceKind::kMetal; }
  engine::platform::GpuContextInfo gpu_context() const override { return {&device, &device}; }
  StatusOr<SurfaceFrame> AcquireFrame(Size size) override {
    acquired++;
    SurfaceFrame frame;
    frame.kind = SurfaceKind::kMetal;
    frame.size = size;
    return frame;
  }
  bool SupportsPlatformViews() const override { return true; }
  void Deactivate() override { deactivated++; }
  bool device_lost() const override { return lost; }

  mutable int device = 0;
  int acquired = 0;
  int deactivated = 0;
  bool lost = false;
};

struct FallbackFixture {
  FallbackFixture() {
    auto gpu_owned = std::make_unique<FakeGpuSurface>();
    auto software_owned = std::make_unique<FakeSoftwareSurface>();
    gpu = gpu_owned.get();
    software = software_owned.get();
    surface = std::make_unique<FallbackSurface>(std::move(gpu_owned), std::move(software_owned));
  }
  FakeGpuSurface* gpu;
  FakeSoftwareSurface* software;
  std::unique_ptr<FallbackSurface> surface;
};

TEST(FallbackSurfaceTest, ForwardsToTheGpuSurfaceWhileItWorks) {
  FallbackFixture f;
  EXPECT_EQ(f.surface->kind(), SurfaceKind::kMetal);
  EXPECT_NE(f.surface->gpu_context().device, nullptr);
  EXPECT_TRUE(f.surface->SupportsPlatformViews());
  ASSERT_TRUE(f.surface->AcquireFrame({10, 10}).ok());
  EXPECT_EQ(f.gpu->acquired, 1);
  EXPECT_EQ(f.software->acquired, 0);
  EXPECT_FALSE(f.surface->software_active());
}

// Rasterizer 建不出 GPU 后端: 停用 GPU surface(一次), 之后的帧来自软件 surface。
TEST(FallbackSurfaceTest, FallsBackWhenAsked) {
  FallbackFixture f;
  EXPECT_TRUE(f.surface->FallBackToSoftware());
  EXPECT_TRUE(f.surface->FallBackToSoftware());
  EXPECT_EQ(f.gpu->deactivated, 1);
  EXPECT_TRUE(f.surface->software_active());
  EXPECT_EQ(f.surface->kind(), SurfaceKind::kSoftware);
  EXPECT_EQ(f.surface->gpu_context().device, nullptr);
  EXPECT_FALSE(f.surface->SupportsPlatformViews());
  ASSERT_TRUE(f.surface->AcquireFrame({10, 10}).ok());
  EXPECT_EQ(f.gpu->acquired, 0);
  EXPECT_EQ(f.software->acquired, 1);
}

// 设备丢失: 下一次取帧时自行回退, 这一帧已经是软件帧(Rasterizer 发现 kind() 变化后换用软件后端)。
TEST(FallbackSurfaceTest, FallsBackWhenTheDeviceIsLost) {
  FallbackFixture f;
  ASSERT_TRUE(f.surface->AcquireFrame({10, 10}).ok());
  f.gpu->lost = true;
  StatusOr<SurfaceFrame> frame = f.surface->AcquireFrame({10, 10});
  ASSERT_TRUE(frame.ok());
  EXPECT_EQ(frame->kind, SurfaceKind::kSoftware);
  EXPECT_EQ(f.gpu->deactivated, 1);
  EXPECT_EQ(f.surface->kind(), SurfaceKind::kSoftware);
}

}  // namespace
}  // namespace momo::embedder
