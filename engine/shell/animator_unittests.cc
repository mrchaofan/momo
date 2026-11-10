#include "engine/shell/animator.h"

#include <memory>

#include "base/thread.h"
#include "base/waitable_event.h"
#include "engine/shell/testing/shell_test_utils.h"
#include "gtest/gtest.h"

namespace momo::engine {
namespace {

using testing::FakeVsyncWaiter;
using testing::ManualTaskRunner;

class RecordingDelegate final : public Animator::Delegate {
 public:
  void OnAnimatorBeginFrame(TimePoint, TimePoint) override {
    begin_frames++;
    if (render_on_begin && animator != nullptr) {
      animator->Render(std::make_unique<graphics::LayerTree>(Size{100, 50}, 2.0));
      if (render_twice) {
        animator->Render(std::make_unique<graphics::LayerTree>(Size{100, 50}, 2.0));
      }
    }
  }
  void OnAnimatorDraw(std::shared_ptr<LayerTreePipeline> pipeline) override { draws++; }

  Animator* animator = nullptr;
  bool render_on_begin = true;
  bool render_twice = false;
  int begin_frames = 0;
  int draws = 0;
};

class AnimatorTest : public ::testing::Test {
 protected:
  AnimatorTest() : ui_(std::make_shared<ManualTaskRunner>()) {
    auto waiter = std::make_unique<FakeVsyncWaiter>(ui_, &await_count_);
    waiter_ = waiter.get();
    animator_ = std::make_unique<Animator>(delegate_, ui_, std::move(waiter));
    delegate_.animator = animator_.get();
  }

  // 消费一个已入队的 LayerTree(模拟 raster 线程)。
  PipelineConsumeResult ConsumeOne() {
    return animator_->pipeline()->Consume([](std::unique_ptr<FrameItem>) {});
  }

  std::shared_ptr<ManualTaskRunner> ui_;
  RecordingDelegate delegate_;
  int await_count_ = 0;
  FakeVsyncWaiter* waiter_ = nullptr;
  std::unique_ptr<Animator> animator_;
};

TEST_F(AnimatorTest, IdleAnimatorDoesNotRequestVsync) {
  EXPECT_FALSE(waiter_->is_waiting());
  EXPECT_EQ(await_count_, 0);
}

TEST_F(AnimatorTest, RequestsAreCoalescedIntoOneFrame) {
  animator_->RequestFrame();
  animator_->RequestFrame();
  animator_->RequestFrame();
  EXPECT_EQ(await_count_, 1);
  EXPECT_TRUE(animator_->frame_scheduled());

  waiter_->Fire();
  EXPECT_EQ(delegate_.begin_frames, 1);
  EXPECT_EQ(delegate_.draws, 1);
  EXPECT_FALSE(animator_->frame_scheduled());
  EXPECT_FALSE(waiter_->is_waiting());
  EXPECT_EQ(animator_->stats().frames_rendered, 1u);
}

TEST_F(AnimatorTest, RenderedLayerTreeCarriesTimings) {
  animator_->RequestFrame();
  waiter_->Fire();
  bool checked = false;
  animator_->pipeline()->Consume([&](std::unique_ptr<FrameItem> item) {
    ASSERT_NE(item, nullptr);
    ASSERT_EQ(item->layer_trees.size(), 1u);
    const std::unique_ptr<graphics::LayerTree>& tree = item->layer_trees[0];
    EXPECT_EQ(tree->frame_size(), (Size{100, 50}));
    const auto& t = tree->timings();
    EXPECT_LE(t.vsync_start, t.frame_target);
    EXPECT_LE(t.build_start, t.build_end);
    checked = true;
  });
  EXPECT_TRUE(checked);
}

TEST_F(AnimatorTest, RenderOutsideFrameIsDropped) {
  animator_->Render(std::make_unique<graphics::LayerTree>(Size{1, 1}, 1.0));
  EXPECT_EQ(animator_->stats().renders_dropped, 1u);
  EXPECT_EQ(delegate_.draws, 0);
  EXPECT_EQ(animator_->pipeline()->queued(), 0u);
}

TEST_F(AnimatorTest, SecondRenderInSameFrameIsDropped) {
  delegate_.render_twice = true;
  animator_->RequestFrame();
  waiter_->Fire();
  EXPECT_EQ(animator_->stats().frames_rendered, 1u);
  EXPECT_EQ(animator_->stats().renders_dropped, 1u);
  EXPECT_EQ(animator_->pipeline()->queued(), 1u);
}

TEST_F(AnimatorTest, FrameWithoutRenderReleasesPipelineSlot) {
  delegate_.render_on_begin = false;
  animator_->RequestFrame();
  waiter_->Fire();
  EXPECT_EQ(delegate_.begin_frames, 1);
  EXPECT_EQ(animator_->pipeline()->available_slots(), Animator::kPipelineDepth);
}

TEST_F(AnimatorTest, BackpressureSkipsVsyncUntilRasterCatchesUp) {
  // raster 不消费: 两帧填满深度 2 的管道。
  for (int i = 0; i < 2; ++i) {
    animator_->RequestFrame();
    waiter_->Fire();
  }
  EXPECT_EQ(delegate_.begin_frames, 2);

  // 第三帧: 管道已满, 跳过且不驱动 JS, 但仍保持等待下一次 vsync。
  animator_->RequestFrame();
  waiter_->Fire();
  EXPECT_EQ(delegate_.begin_frames, 2);
  EXPECT_EQ(animator_->stats().frames_skipped, 1u);
  EXPECT_TRUE(waiter_->is_waiting());
  EXPECT_TRUE(animator_->frame_scheduled());

  // raster 消费一帧后, 下一次 vsync 恢复出帧。
  EXPECT_EQ(ConsumeOne(), PipelineConsumeResult::kMoreAvailable);
  waiter_->Fire();
  EXPECT_EQ(delegate_.begin_frames, 3);
  EXPECT_FALSE(waiter_->is_waiting());
}

TEST_F(AnimatorTest, StopDefersRequestsUntilStart) {
  animator_->Stop();
  animator_->RequestFrame();
  EXPECT_EQ(await_count_, 0);

  animator_->Start();
  EXPECT_EQ(await_count_, 1);
  waiter_->Fire();
  EXPECT_EQ(delegate_.begin_frames, 1);
}

TEST_F(AnimatorTest, VsyncArrivingWhileStoppedIsReplayedOnStart) {
  animator_->RequestFrame();
  animator_->Stop();
  waiter_->Fire();
  EXPECT_EQ(delegate_.begin_frames, 0);

  animator_->Start();
  waiter_->Fire();
  EXPECT_EQ(delegate_.begin_frames, 1);
}

TEST_F(AnimatorTest, ReplacingWaiterTransfersPendingRequest) {
  animator_->RequestFrame();
  int new_awaits = 0;
  auto replacement = std::make_unique<FakeVsyncWaiter>(ui_, &new_awaits);
  FakeVsyncWaiter* raw = replacement.get();
  animator_->SetVsyncWaiter(std::move(replacement));
  waiter_ = nullptr;  // 旧 waiter 已销毁
  EXPECT_EQ(new_awaits, 1);
  raw->Fire();
  EXPECT_EQ(delegate_.begin_frames, 1);
}

// ---- 真实线程上的 vsync waiter ----

TEST(VsyncWaiterTest, TimerWaiterFiresOnUiThreadAndCoalesces) {
  Thread ui("test.ui");
  auto runner = ui.GetTaskRunner();
  WaitableEvent fired;
  std::atomic<int> calls{0};
  std::unique_ptr<TimerVsyncWaiter> waiter;

  TaskRunner::RunNowOrPostTask(*runner, [&] {
    waiter = std::make_unique<TimerVsyncWaiter>(runner, 120.0);
    EXPECT_EQ(waiter->interval(), std::chrono::duration_cast<TimeDelta>(
                                      std::chrono::duration<double>(1.0 / 120.0)));
    auto cb = [&, runner](TimePoint start, TimePoint target) {
      EXPECT_TRUE(runner->RunsTasksOnCurrentThread());
      EXPECT_LT(start, target);
      calls++;
      fired.Signal();
    };
    waiter->AsyncWaitForVsync(cb);
    waiter->AsyncWaitForVsync(cb);  // 合并
  });
  fired.Wait();

  WaitableEvent destroyed;
  runner->PostDelayedTask(
      [&] {
        waiter.reset();
        destroyed.Signal();
      },
      std::chrono::milliseconds(30));
  destroyed.Wait();
  EXPECT_EQ(calls.load(), 1);
  ui.Join();
}

// VsyncSource 归属 platform 线程: 请求从 ui 投递过去调用, 回调回到 ui。
TEST(VsyncWaiterTest, PlatformWaiterRequestsOnSourceThreadAndFiresOnUi) {
  Thread platform("test.platform");
  Thread ui("test.ui");
  auto platform_runner = platform.GetTaskRunner();
  auto ui_runner = ui.GetTaskRunner();

  ObjectPtr<testing::FakeVsyncSource> source;
  WaitableEvent created;
  platform_runner->PostTask([&] {
    source = MakeObject<testing::FakeVsyncSource>();  // 默认归属 platform 线程的循环
    created.Signal();
  });
  created.Wait();
  WeakPtr<platform::VsyncSource> weak(source.get());

  WaitableEvent fired;
  std::unique_ptr<PlatformVsyncWaiter> waiter;
  ui_runner->PostTask([&] {
    waiter = std::make_unique<PlatformVsyncWaiter>(ui_runner, weak);
    waiter->AsyncWaitForVsync([&, ui_runner](TimePoint, TimePoint) {
      EXPECT_TRUE(ui_runner->RunsTasksOnCurrentThread());
      fired.Signal();
    });
  });
  fired.Wait();

  WaitableEvent done;
  platform_runner->PostTask([&] {
    EXPECT_EQ(source->requests.load(), 1);
    EXPECT_FALSE(source->called_off_thread.load());
    source.reset();  // DeleteLater: 在 platform 循环安全点析构
    done.Signal();
  });
  done.Wait();
  ui_runner->PostTask([&] { waiter.reset(); });
  ui.Join();
  platform.Join();
}

TEST(VsyncWaiterTest, RequestAfterSourceDeletedIsDropped) {
  Thread platform("test.platform");
  auto platform_runner = platform.GetTaskRunner();
  auto ui = std::make_shared<ManualTaskRunner>();

  std::atomic<int> destroyed{0};
  WeakPtr<platform::VsyncSource> weak;
  WaitableEvent ready;
  platform_runner->PostTask([&] {
    auto source = MakeObject<testing::FakeVsyncSource>([&] { destroyed++; });
    weak = WeakPtr<platform::VsyncSource>(source.get());
    ready.Signal();
    // source 在此释放 → DeleteLater, 本轮结束时析构。
  });
  ready.Wait();
  ASSERT_TRUE(testing::WaitFor([&] { return destroyed.load() == 1; }));

  bool called = false;
  PlatformVsyncWaiter waiter(ui, weak);
  waiter.AsyncWaitForVsync([&](TimePoint, TimePoint) { called = true; });
  // 请求投递到 platform 线程后判活失败, 不会回调。
  WaitableEvent flushed;
  platform_runner->PostTask([&] { flushed.Signal(); });
  flushed.Wait();
  ui->RunUntilIdle();
  EXPECT_FALSE(called);
  platform.Join();
}

TEST(VsyncWaiterTest, CallbackAfterWaiterDestroyedIsDropped) {
  Thread platform("test.platform");
  auto platform_runner = platform.GetTaskRunner();
  auto ui = std::make_shared<ManualTaskRunner>();

  ObjectPtr<testing::FakeVsyncSource> source;
  WaitableEvent created;
  platform_runner->PostTask([&] {
    source = MakeObject<testing::FakeVsyncSource>();
    created.Signal();
  });
  created.Wait();

  bool called = false;
  {
    PlatformVsyncWaiter waiter(ui, WeakPtr<platform::VsyncSource>(source.get()));
    waiter.AsyncWaitForVsync([&](TimePoint, TimePoint) { called = true; });
  }
  // 等平台回调把任务投递到 ui。
  ASSERT_TRUE(testing::WaitFor([&] { return ui->pending() > 0; }));
  ui->RunUntilIdle();
  EXPECT_FALSE(called);

  WaitableEvent released;
  platform_runner->PostTask([&] {
    source.reset();
    released.Signal();
  });
  released.Wait();
  platform.Join();
}

}  // namespace
}  // namespace momo::engine
