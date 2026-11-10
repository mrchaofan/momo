#include "base/native_message_pump.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "base/event_loop.h"
#include "base/object.h"
#include "base/testing/mock_native_loop.h"
#include "gtest/gtest.h"

namespace momo {
namespace {

using namespace std::chrono_literals;

class Probe final : public Object {
 public:
  explicit Probe(int* destroyed) : destroyed_(destroyed) {}
  ~Probe() override { (*destroyed_)++; }

 private:
  int* destroyed_;
};

// OS 主线程的 EventLoop 跑在(模拟的)原生消息循环之上。
class NativeMessagePumpTest : public ::testing::Test {
 protected:
  NativeMessagePumpTest()
      : loop_(EventLoop::Create(std::make_unique<NativeMessagePump>(native_))),
        runner_(loop_->GetTaskRunner()) {
    loop_->BindToCurrentThread();
  }
  ~NativeMessagePumpTest() override {
    if (loop_) {
      loop_->Terminate();
      loop_->RunUntilIdle();
    }
  }

  testing::MockNativeLoop native_;
  std::shared_ptr<EventLoop> loop_;
  std::shared_ptr<TaskRunner> runner_;
};

TEST_F(NativeMessagePumpTest, RunsImmediateAndDelayedTasksThenQuitsNativeLoop) {
  std::vector<int> order;
  runner_->PostDelayedTask([&] { order.push_back(3); loop_->Terminate(); }, 20ms);
  runner_->PostDelayedTask([&] { order.push_back(2); }, 5ms);
  runner_->PostTask([&] { order.push_back(1); });
  loop_->Run();
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_TRUE(loop_->has_exited());
  EXPECT_EQ(native_.run_count(), 1);
  EXPECT_EQ(native_.quit_count(), 1);
}

TEST_F(NativeMessagePumpTest, CrossThreadPostWakesTheNativeLoop) {
  std::atomic<bool> ran{false};
  std::thread poster([&] {
    std::this_thread::sleep_for(5ms);
    runner_->PostTask([&] {
      ran = true;
      loop_->Terminate();
    });
  });
  loop_->Run();
  poster.join();
  EXPECT_TRUE(ran.load());
}

TEST_F(NativeMessagePumpTest, NestedNativeLoopDefersDeletionUntilOuterTaskReturns) {
  int destroyed = 0;
  auto* probe = new Probe(&destroyed);
  int destroyed_inside_nested = -1;
  int destroyed_after_nested = -1;

  runner_->PostTask([&] {
    // 外层任务开启一个"模态"原生循环(pump 会在其中重入 DoWork)。
    runner_->PostTask([&] {
      probe->DeleteLater();
      runner_->PostTask([&] {
        destroyed_inside_nested = destroyed;  // 仍在嵌套中: 未析构
        native_.Quit();           // 结束模态
      });
    });
    native_.Run();
    destroyed_after_nested = destroyed;  // 外层任务尚未返回: 仍未析构
    runner_->PostTask([&] { loop_->Terminate(); });
  });
  loop_->Run();

  EXPECT_EQ(destroyed_inside_nested, 0);
  EXPECT_EQ(destroyed_after_nested, 0);
  EXPECT_EQ(destroyed, 1) << "deleted at the outer safe point";
}

TEST_F(NativeMessagePumpTest, DeleteLaterFromAnotherThreadIsProcessedByIdleLoop) {
  int destroyed = 0;
  auto* probe = new Probe(&destroyed);
  std::thread releaser([probe] {
    std::this_thread::sleep_for(5ms);
    probe->DeleteLater();
  });
  // 轮询检查: 投递的检查任务若看到已析构就结束循环。
  std::function<void()> poll = [&] {
    if (destroyed == 1) {
      loop_->Terminate();
    } else {
      runner_->PostDelayedTask([&] { poll(); }, 1ms);
    }
  };
  runner_->PostTask([&] { poll(); });
  loop_->Run();
  releaser.join();
  EXPECT_EQ(destroyed, 1);
}

TEST_F(NativeMessagePumpTest, PumpOutlivedByNativeTasksIsHarmless) {
  // 循环销毁后, native loop 中残留的唤醒任务变为空操作。
  runner_->PostTask([] {});
  loop_->Terminate();
  loop_->RunUntilIdle();
  runner_.reset();
  loop_.reset();
  native_.PostTask([this] { native_.Quit(); });
  native_.Run();  // 执行残留的 DoWork 任务, 不崩溃
  SUCCEED();
}

}  // namespace
}  // namespace momo
