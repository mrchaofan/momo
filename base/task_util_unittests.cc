#include "base/task_util.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "base/event_loop.h"
#include "base/object.h"
#include "base/thread.h"
#include "base/waitable_event.h"
#include "gtest/gtest.h"

namespace momo {
namespace {

TEST(TaskUtilTest, PostTaskAndWaitRunsOnTargetThread) {
  Thread worker("test.worker");
  auto runner = worker.GetTaskRunner();
  bool on_target = false;
  EXPECT_TRUE(PostTaskAndWait(*runner, [&] { on_target = runner->RunsTasksOnCurrentThread(); }));
  EXPECT_TRUE(on_target);
}

TEST(TaskUtilTest, PostTaskAndWaitRunsInlineOnSameThread) {
  auto loop = EventLoop::Create();
  loop->BindToCurrentThread();
  bool ran = false;
  EXPECT_TRUE(PostTaskAndWait(*loop->GetTaskRunner(), [&] { ran = true; }));
  EXPECT_TRUE(ran);
  loop->Terminate();
  loop->RunUntilIdle();
}

TEST(TaskUtilTest, PostTaskAndWaitDoesNotDeadlockOnTerminatedThread) {
  Thread worker("test.worker");
  auto runner = worker.GetTaskRunner();
  worker.Join();
  bool ran = false;
  EXPECT_FALSE(PostTaskAndWait(*runner, [&] { ran = true; }));
  EXPECT_FALSE(ran);
}

TEST(TaskUtilTest, ReplyWithResultRunsOnReplyThread) {
  Thread worker("test.worker");
  auto loop = EventLoop::Create();
  loop->BindToCurrentThread();
  auto reply_runner = loop->GetTaskRunner();

  std::string result;
  bool reply_on_main = false;
  PostTaskAndReplyWithResult(
      *worker.GetTaskRunner(), [] { return std::string("from worker"); }, reply_runner,
      [&](std::string value) {
        result = std::move(value);
        reply_on_main = reply_runner->RunsTasksOnCurrentThread();
        loop->Terminate();
      });
  loop->Run();
  EXPECT_EQ(result, "from worker");
  EXPECT_TRUE(reply_on_main);
}

TEST(TaskUtilTest, PostTaskAndReplyOrdersTaskBeforeReply) {
  Thread worker("test.worker");
  auto loop = EventLoop::Create();
  loop->BindToCurrentThread();
  std::atomic<bool> task_ran{false};
  bool seen_in_reply = false;
  PostTaskAndReply(
      *worker.GetTaskRunner(), [&] { task_ran = true; }, loop->GetTaskRunner(),
      [&] {
        seen_in_reply = task_ran.load();
        loop->Terminate();
      });
  loop->Run();
  EXPECT_TRUE(seen_in_reply);
}

class Counter final : public Object {
 public:
  int value = 0;
};

TEST(TaskUtilTest, PostToObjectRunsOnlyWhileAlive) {
  auto loop = EventLoop::Create();
  loop->BindToCurrentThread();
  auto counter = MakeObject<Counter>();
  WeakPtr<Counter> weak(counter.get());
  PostToObject(weak, [](Counter& c) { c.value++; });
  loop->RunUntilIdle();
  EXPECT_EQ(counter->value, 1);

  counter.reset();          // DeleteLater
  loop->RunUntilIdle();     // 已析构
  bool ran = false;
  PostToObject(weak, [&](Counter&) { ran = true; });
  loop->RunUntilIdle();
  EXPECT_FALSE(ran);

  PostToObject(WeakPtr<Counter>(), [&](Counter&) { ran = true; });  // 未绑定: 无操作
  loop->Terminate();
  loop->RunUntilIdle();
  EXPECT_FALSE(ran);
}

TEST(TaskUtilTest, InvalidateWeakPtrsCutsExistingReferences) {
  auto loop = EventLoop::Create();
  loop->BindToCurrentThread();
  struct Closable final : Object {
    void Close() { InvalidateWeakPtrs(); }
  };
  Closable obj;
  WeakPtr<Closable> before(&obj);
  obj.Close();
  WeakPtr<Closable> after(&obj);
  EXPECT_EQ(before.get(), nullptr);
  EXPECT_EQ(after.get(), &obj);
  loop->Terminate();
  loop->RunUntilIdle();
}

}  // namespace
}  // namespace momo
