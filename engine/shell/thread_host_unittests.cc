#include "engine/shell/thread_host.h"

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/waitable_event.h"
#include "gtest/gtest.h"

namespace momo::engine {
namespace {

class FakePlatformRunner final : public TaskRunner {
 public:
  void PostTask(Closure) override {}
  void PostDelayedTask(Closure, TimeDelta) override {}
  bool RunsTasksOnCurrentThread() const override { return false; }
};

TEST(ThreadHostTest, ProvidesDistinctRunnersOnSeparateThreads) {
  ThreadHost host("t");
  auto platform = std::make_shared<FakePlatformRunner>();
  TaskRunners runners = host.MakeTaskRunners(platform);
  ASSERT_TRUE(runners.IsValid());
  EXPECT_EQ(runners.label, "t");
  EXPECT_EQ(runners.platform, platform);

  std::thread::id ids[3];
  std::shared_ptr<TaskRunner> list[3] = {runners.ui, runners.raster, runners.io};
  for (int i = 0; i < 3; ++i) {
    WaitableEvent done;
    list[i]->PostTask([&, i] {
      ids[i] = std::this_thread::get_id();
      EXPECT_TRUE(list[i]->RunsTasksOnCurrentThread());
      for (int j = 0; j < 3; ++j) {
        if (j != i) EXPECT_FALSE(list[j]->RunsTasksOnCurrentThread());
      }
      done.Signal();
    });
    done.Wait();
  }
  EXPECT_NE(ids[0], ids[1]);
  EXPECT_NE(ids[1], ids[2]);
  EXPECT_NE(ids[0], ids[2]);
  EXPECT_NE(ids[0], std::this_thread::get_id());
}

TEST(ThreadHostTest, JoinOrderLetsUiAndRasterPostFinalWorkToIo) {
  std::mutex mutex;
  std::vector<std::string> ran;
  {
    ThreadHost host;
    TaskRunners runners = host.MakeTaskRunners(std::make_shared<FakePlatformRunner>());
    // ui/raster 的收尾任务向 io 投递: io 最后 join, 所以这些任务仍会执行。
    runners.ui->PostTask([&, io = runners.io] {
      io->PostTask([&] { std::lock_guard<std::mutex> l(mutex); ran.push_back("from-ui"); });
    });
    runners.raster->PostTask([&, io = runners.io] {
      io->PostTask([&] { std::lock_guard<std::mutex> l(mutex); ran.push_back("from-raster"); });
    });
    host.Join();
    EXPECT_TRUE(host.joined());
    host.Join();  // 幂等
  }
  EXPECT_EQ(ran.size(), 2u);
}

}  // namespace
}  // namespace momo::engine
