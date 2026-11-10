#include "base/event_loop.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include "base/status.h"
#include "base/thread.h"
#include "base/waitable_event.h"
#include "gtest/gtest.h"

namespace momo {
namespace {

using namespace std::chrono_literals;

TEST(ThreadTest, RunsTasksOnItsOwnThreadInFifoOrder) {
  Thread thread("momo.test");
  auto runner = thread.GetTaskRunner();
  EXPECT_FALSE(runner->RunsTasksOnCurrentThread());

  std::vector<int> order;
  WaitableEvent done;
  for (int i = 0; i < 100; ++i) {
    runner->PostTask([&order, i, runner] {
      EXPECT_TRUE(runner->RunsTasksOnCurrentThread());
      order.push_back(i);
    });
  }
  runner->PostTask([&done] { done.Signal(); });
  done.Wait();
  ASSERT_EQ(order.size(), 100u);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(order[i], i);
  }
}

TEST(ThreadTest, DelayedTasksRunInDeadlineOrder) {
  Thread thread("momo.test");
  auto runner = thread.GetTaskRunner();
  std::mutex mutex;
  std::vector<int> order;
  WaitableEvent done;
  auto record = [&](int v) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(v);
  };
  runner->PostDelayedTask([&] { record(3); done.Signal(); }, 60ms);
  runner->PostDelayedTask([&] { record(2); }, 30ms);
  runner->PostTask([&] { record(1); });
  done.Wait();
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(ThreadTest, JoinDrainsReadyTasksAndDropsFutureOnes) {
  std::atomic<int> ran{0};
  std::atomic<bool> delayed_ran{false};
  {
    Thread thread("momo.test");
    auto runner = thread.GetTaskRunner();
    WaitableEvent gate;
    runner->PostTask([&gate] { gate.Wait(); });  // 堵住线程, 让后续任务在 Join 前排队
    for (int i = 0; i < 10; ++i) {
      runner->PostTask([&ran] { ran++; });
    }
    runner->PostDelayedTask([&delayed_ran] { delayed_ran = true; }, 10s);
    gate.Signal();
    thread.Join();
    EXPECT_TRUE(thread.joined());

    // join 之后投递: 丢弃, 不崩溃。
    runner->PostTask([&ran] { ran += 1000; });
  }
  EXPECT_EQ(ran.load(), 10);
  EXPECT_FALSE(delayed_ran.load());
}

TEST(ThreadTest, RunNowOrPostTaskRunsInlineOnTargetThread) {
  Thread thread("momo.test");
  auto runner = thread.GetTaskRunner();
  WaitableEvent done;
  runner->PostTask([&] {
    bool inline_ran = false;
    TaskRunner::RunNowOrPostTask(*runner, [&inline_ran] { inline_ran = true; });
    EXPECT_TRUE(inline_ran);
    done.Signal();
  });
  done.Wait();
}

TEST(EventLoopTest, TerminateBeforeRunDrainsAndReturns) {
  auto loop = EventLoop::Create();
  bool ran = false;
  loop->PostTask([&ran] { ran = true; }, Clock::now());
  loop->Terminate();
  loop->Run();
  EXPECT_TRUE(ran) << "tasks posted before Terminate are drained";
  EXPECT_TRUE(loop->has_exited());
  EXPECT_EQ(EventLoop::Current(), nullptr) << "exited loop unbinds from the thread";
}

TEST(EventLoopTest, BindingSetsCurrentAndThreadAffinity) {
  auto loop = EventLoop::Create();
  EXPECT_FALSE(loop->RunsTasksOnCurrentThread());
  loop->BindToCurrentThread();
  EXPECT_EQ(EventLoop::Current(), loop.get());
  EXPECT_TRUE(loop->RunsTasksOnCurrentThread());
  loop->Terminate();
  loop->RunUntilIdle();
  EXPECT_EQ(EventLoop::Current(), nullptr);
}

TEST(EventLoopTest, TasksPostedDuringAnIterationRunInTheNextOne) {
  auto loop = EventLoop::Create();
  loop->BindToCurrentThread();
  std::vector<int> order;
  auto runner = loop->GetTaskRunner();
  runner->PostTask([&] {
    order.push_back(1);
    runner->PostTask([&] { order.push_back(3); });
  });
  runner->PostTask([&] { order.push_back(2); });
  EXPECT_EQ(loop->RunUntilIdle(), 3u);
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  loop->Terminate();
  loop->RunUntilIdle();
}

TEST(EventLoopTest, RunUntilIdleDoesNotWaitForDelayedTasks) {
  auto loop = EventLoop::Create();
  bool ran = false;
  loop->GetTaskRunner()->PostDelayedTask([&] { ran = true; }, 10s);
  EXPECT_EQ(loop->RunUntilIdle(), 0u);
  EXPECT_FALSE(ran);
  loop->Terminate();
  loop->RunUntilIdle();
  EXPECT_FALSE(ran) << "not-yet-due tasks are dropped at exit";
  EXPECT_TRUE(loop->has_exited());
}

TEST(EventLoopTest, PostAfterExitIsDropped) {
  auto loop = EventLoop::Create();
  loop->Terminate();
  loop->RunUntilIdle();
  auto token = std::make_shared<int>(0);
  std::weak_ptr<int> weak = token;
  loop->GetTaskRunner()->PostTask([token = std::move(token)] {});
  EXPECT_TRUE(weak.expired());
}

TEST(ClosureTest, SupportsMoveOnlyCaptures) {
  auto value = std::make_unique<int>(42);
  int seen = 0;
  Closure closure([v = std::move(value), &seen] { seen = *v; });
  Closure moved = std::move(closure);
  EXPECT_FALSE(static_cast<bool>(closure));  // NOLINT(bugprone-use-after-move)
  moved();
  EXPECT_EQ(seen, 42);
}

TEST(StatusTest, StatusOrCarriesValueOrError) {
  StatusOr<int> ok_value = 7;
  ASSERT_TRUE(ok_value.ok());
  EXPECT_EQ(*ok_value, 7);
  StatusOr<int> error = NotFoundError("missing");
  EXPECT_FALSE(error.ok());
  EXPECT_EQ(error.status().code(), StatusCode::kNotFound);
  EXPECT_EQ(error.status().ToString(), "NOT_FOUND: missing");

  auto chain = [](bool fail) -> StatusOr<int> {
    auto inner = [fail]() -> StatusOr<int> {
      if (fail) return InternalError("boom");
      return 1;
    };
    MOMO_ASSIGN_OR_RETURN(int v, inner());
    return v + 1;
  };
  EXPECT_EQ(*chain(false), 2);
  EXPECT_EQ(chain(true).status().code(), StatusCode::kInternal);
}

}  // namespace
}  // namespace momo
