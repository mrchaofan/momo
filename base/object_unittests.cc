#include "base/object.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "base/event_loop.h"
#include "base/thread.h"
#include "base/waitable_event.h"
#include "gtest/gtest.h"

namespace momo {
namespace {

// 记录析构线程的探针对象。
class Probe final : public Object {
 public:
  explicit Probe(std::atomic<int>* destroyed, std::thread::id* destroyed_on = nullptr)
      : destroyed_(destroyed), destroyed_on_(destroyed_on) {}
  Probe(std::shared_ptr<TaskRunner> affinity, std::atomic<int>* destroyed,
        std::thread::id* destroyed_on)
      : Object(std::move(affinity)), destroyed_(destroyed), destroyed_on_(destroyed_on) {}
  ~Probe() override {
    if (destroyed_on_ != nullptr) {
      *destroyed_on_ = std::this_thread::get_id();
    }
    (*destroyed_)++;
  }

  // 在自己的方法里删除自己, 之后仍访问成员: DeleteLater 保证安全。
  int SelfDestructAndTouch() {
    DeleteLater();
    return value_ + 1;
  }

 private:
  std::atomic<int>* destroyed_;
  std::thread::id* destroyed_on_;
  int value_ = 41;
};

// 测试主线程上的循环。
class ObjectTest : public ::testing::Test {
 protected:
  ObjectTest() : loop_(EventLoop::Create()) { loop_->BindToCurrentThread(); }
  ~ObjectTest() override {
    loop_->Terminate();
    loop_->RunUntilIdle();
  }
  std::shared_ptr<EventLoop> loop_;  // 部分用例需要直接 Terminate, 故不用 ScopedTestEventLoop
};

TEST_F(ObjectTest, DefaultAffinityIsCurrentLoop) {
  std::atomic<int> destroyed{0};
  Probe probe(&destroyed);
  EXPECT_TRUE(probe.IsOnAffinityThread());
}

TEST_F(ObjectTest, DeleteLaterWaitsUntilTheCurrentTaskReturns) {
  std::atomic<int> destroyed{0};
  auto* probe = new Probe(&destroyed);
  WeakPtr<Probe> weak(probe);
  bool alive_in_same_task = false;
  bool alive_in_next_batch_task = false;
  auto runner = loop_->GetTaskRunner();
  runner->PostTask([&] {
    probe->DeleteLater();
    alive_in_same_task = weak.get() != nullptr;
  });
  // 同一轮快照中的后续任务: 析构发生在整轮任务之后。
  runner->PostTask([&] { alive_in_next_batch_task = weak.get() != nullptr; });
  loop_->RunUntilIdle();
  EXPECT_TRUE(alive_in_same_task);
  EXPECT_TRUE(alive_in_next_batch_task);
  EXPECT_EQ(destroyed.load(), 1);
  EXPECT_EQ(weak.get(), nullptr);
}

TEST_F(ObjectTest, DeleteLaterIsIdempotent) {
  std::atomic<int> destroyed{0};
  auto* probe = new Probe(&destroyed);
  probe->DeleteLater();
  probe->DeleteLater();
  EXPECT_TRUE(probe->is_delete_scheduled());
  loop_->RunUntilIdle();
  EXPECT_EQ(destroyed.load(), 1);
}

TEST_F(ObjectTest, ObjectCanDeleteItselfInsideItsOwnMethod) {
  std::atomic<int> destroyed{0};
  auto* probe = new Probe(&destroyed);
  EXPECT_EQ(probe->SelfDestructAndTouch(), 42);
  EXPECT_EQ(destroyed.load(), 0);
  loop_->RunUntilIdle();
  EXPECT_EQ(destroyed.load(), 1);
}

TEST_F(ObjectTest, ReleasingObjectPtrDefersDeletion) {
  std::atomic<int> destroyed{0};
  ObjectPtr<Probe> ptr = MakeObject<Probe>(&destroyed);
  WeakPtr<Probe> weak(ptr.get());
  ptr.reset();
  EXPECT_NE(weak.get(), nullptr) << "not deleted inline";
  loop_->RunUntilIdle();
  EXPECT_EQ(weak.get(), nullptr);
  EXPECT_EQ(destroyed.load(), 1);
}

TEST_F(ObjectTest, ObjectPtrConvertsToBase) {
  std::atomic<int> destroyed{0};
  ObjectPtr<Object> base = MakeObject<Probe>(&destroyed);
  base.reset();
  loop_->RunUntilIdle();
  EXPECT_EQ(destroyed.load(), 1);
}

TEST_F(ObjectTest, TerminateFlushesDeferredDeletes) {
  std::atomic<int> destroyed{0};
  (new Probe(&destroyed))->DeleteLater();
  loop_->Terminate();
  loop_->RunUntilIdle();
  EXPECT_EQ(destroyed.load(), 1);
  EXPECT_TRUE(loop_->has_exited());
}

TEST_F(ObjectTest, DeletionsChainedDuringExitAreAllFlushed) {
  // 析构时又 DeleteLater 另一个对象: 结束清理反复进行直到为空。
  struct Chain final : Object {
    Chain(std::atomic<int>* d, Object* next) : destroyed(d), next(next) {}
    ~Chain() override {
      (*destroyed)++;
      if (next != nullptr) {
        next->DeleteLater();
      }
    }
    std::atomic<int>* destroyed;
    Object* next;
  };
  std::atomic<int> destroyed{0};
  auto* last = new Chain(&destroyed, nullptr);
  auto* middle = new Chain(&destroyed, last);
  (new Chain(&destroyed, middle))->DeleteLater();
  loop_->Terminate();
  loop_->RunUntilIdle();
  EXPECT_EQ(destroyed.load(), 3);
}

TEST_F(ObjectTest, DeleteLaterAfterExitOnOwnThreadDeletesImmediately) {
  std::atomic<int> destroyed{0};
  auto* probe = new Probe(&destroyed);
  loop_->Terminate();
  loop_->RunUntilIdle();
  probe->DeleteLater();
  EXPECT_EQ(destroyed.load(), 1);
}

// ---- 跨线程 ----

TEST(ObjectThreadTest, ReleasedFromAnotherThreadIsDeletedOnAffinityThread) {
  Thread worker("test.worker");
  auto runner = worker.GetTaskRunner();
  std::atomic<int> destroyed{0};
  std::thread::id destroyed_on;
  ObjectPtr<Probe> ptr = MakeObject<Probe>(runner, &destroyed, &destroyed_on);
  ptr.reset();  // 在测试主线程释放: DeleteLater 唤醒空闲的 worker 循环
  for (int i = 0; i < 1000 && destroyed.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(destroyed.load(), 1);
  std::thread::id worker_id;
  WaitableEvent got;
  runner->PostTask([&] {
    worker_id = std::this_thread::get_id();
    got.Signal();
  });
  got.Wait();
  EXPECT_EQ(destroyed_on, worker_id);
}

TEST(ObjectThreadTest, ThreadJoinFlushesObjectsMarkedByItsLastTasks) {
  std::atomic<int> destroyed{0};
  std::thread::id destroyed_on;
  std::thread::id worker_id;
  {
    Thread worker("test.worker");
    auto runner = worker.GetTaskRunner();
    WaitableEvent gate;
    runner->PostTask([&] {
      worker_id = std::this_thread::get_id();
      gate.Wait();
      (new Probe(&destroyed, &destroyed_on))->DeleteLater();  // 默认归属 worker 的循环
    });
    gate.Signal();
    worker.Join();
  }
  EXPECT_EQ(destroyed.load(), 1);
  EXPECT_EQ(destroyed_on, worker_id);
}

TEST(ObjectThreadTest, WeakPtrIsCheckedOnAffinityThreadAfterPosting) {
  Thread worker("test.worker");
  auto runner = worker.GetTaskRunner();
  std::atomic<int> destroyed{0};
  ObjectPtr<Probe> ptr = MakeObject<Probe>(runner, &destroyed, nullptr);
  WeakPtr<Probe> weak(ptr.get());
  ptr.reset();
  // 另一线程拿着弱引用: 只能投递到归属线程再判活。
  bool alive = true;
  for (int i = 0; i < 200 && alive; ++i) {
    WaitableEvent done;
    weak.task_runner()->PostTask([&] {
      alive = weak.get() != nullptr;
      done.Signal();
    });
    done.Wait();
  }
  EXPECT_FALSE(alive);
}

}  // namespace
}  // namespace momo
