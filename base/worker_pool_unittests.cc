#include "base/worker_pool.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "base/waitable_event.h"
#include "gtest/gtest.h"

namespace momo {
namespace {

TEST(WorkerPoolTest, RunsAllTasksOnWorkerThreads) {
  WorkerPool pool("test", 4);
  EXPECT_EQ(pool.thread_count(), 4u);
  std::atomic<int> count{0};
  std::mutex mutex;
  std::set<std::thread::id> ids;
  const auto main_id = std::this_thread::get_id();
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(pool.PostTask([&] {
      EXPECT_NE(std::this_thread::get_id(), main_id);
      std::lock_guard<std::mutex> lock(mutex);
      ids.insert(std::this_thread::get_id());
      count++;
    }));
  }
  pool.Shutdown();
  EXPECT_EQ(count.load(), 200);
  EXPECT_LE(ids.size(), 4u);
}

TEST(WorkerPoolTest, ShutdownDrainsQueuedTasks) {
  WorkerPool pool("test", 1);
  WaitableEvent release;
  std::atomic<int> count{0};
  pool.PostTask([&] { release.Wait(); count++; });
  for (int i = 0; i < 10; ++i) {
    pool.PostTask([&] { count++; });
  }
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    release.Signal();
  });
  pool.Shutdown();  // 阻塞直到排空
  releaser.join();
  EXPECT_EQ(count.load(), 11);
}

TEST(WorkerPoolTest, PostAfterShutdownIsDroppedAndIdempotent) {
  WorkerPool pool("test", 2);
  pool.Shutdown();
  pool.Shutdown();
  auto token = std::make_shared<int>(0);
  std::weak_ptr<int> weak = token;
  EXPECT_FALSE(pool.PostTask([token = std::move(token)] {}));
  EXPECT_TRUE(weak.expired());  // 被丢弃的闭包已析构
}

TEST(WorkerPoolTest, DefaultThreadCountIsPositive) {
  WorkerPool pool;
  EXPECT_GE(pool.thread_count(), 1u);
}

TEST(WorkerPoolTest, DelayedTasksRunAfterDelay) {
  WorkerPool pool("test", 2);
  WaitableEvent done;
  const TimePoint posted = Clock::now();
  TimePoint ran;
  ASSERT_TRUE(pool.PostDelayedTask(
      [&] {
        ran = Clock::now();
        done.Signal();
      },
      std::chrono::milliseconds(30)));
  done.Wait();
  EXPECT_GE(ran - posted, std::chrono::milliseconds(30));
}

TEST(WorkerPoolTest, EarlierDelayedTaskWakesSleepingWorker) {
  WorkerPool pool("test", 1);
  WaitableEvent done;
  pool.PostDelayedTask([] {}, std::chrono::hours(1));  // worker 进入长等待
  const TimePoint posted = Clock::now();
  pool.PostDelayedTask([&] { done.Signal(); }, std::chrono::milliseconds(10));
  done.Wait();
  EXPECT_LT(Clock::now() - posted, std::chrono::seconds(10));
}

TEST(WorkerPoolTest, ShutdownDropsPendingDelayedTasks) {
  auto counter = std::make_shared<int>(0);
  {
    WorkerPool pool("test", 1);
    pool.PostDelayedTask([counter] { ++*counter; }, std::chrono::hours(1));
    EXPECT_EQ(counter.use_count(), 2);
    pool.Shutdown();
    EXPECT_EQ(counter.use_count(), 1) << "dropped task must be destroyed by Shutdown";
    EXPECT_FALSE(pool.PostDelayedTask([] {}, std::chrono::milliseconds(1)));
  }
  EXPECT_EQ(*counter, 0);
}

}  // namespace
}  // namespace momo
