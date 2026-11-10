#pragma once

// Object: 全项目的框架对象基类, 统一管理线程归属与生命周期(参考 QObject / NSObject, docs/12 §2)。
//
// 1) 线程归属(affinity): 构造时绑定到当前线程的 EventLoop(或显式指定的 TaskRunner)。
//    规则: 对象的方法只在归属线程调用;其他线程经 task_runner()->PostTask 投递, 并用 WeakPtr
//    在归属线程上判活。析构也必须发生在归属线程。
// 2) 延迟析构: DeleteLater() 可在任意线程调用(幂等)。对象在归属线程的事件循环中、本轮迭代的任务
//    全部返回且栈上无任务时析构;嵌套循环中不析构;循环结束时必定清理。因此对象可以安全地在自己的
//    回调栈里被"删除"。
// 3) 所有权: 用 ObjectPtr<T>(独占, 不共享)。释放 ObjectPtr 不会立即 delete, 而是 DeleteLater。
//    也允许在归属线程上直接 delete / 用 std::unique_ptr 持有(立即析构的场景)。
//    已被 ObjectPtr 持有的对象不要再手动 DeleteLater/delete。
// 4) 弱引用: WeakPtr<T>(参考 QPointer), 对象析构(或 InvalidateWeakPtrs)后失效。可随任务投递到
//    任意线程, 但只能在归属线程解引用。PostToObject(weak, fn) = 投递到归属线程、判活后执行。
//    这是项目内唯一的弱引用机制: 需要弱引用的类继承 Object。

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

#include "base/logging.h"
#include "base/macros.h"
#include "base/task_runner.h"

namespace momo {

template <typename T>
class WeakPtr;

class Object {
 public:
  virtual ~Object();

  // 归属线程的 runner。
  const std::shared_ptr<TaskRunner>& task_runner() const { return task_runner_; }
  bool IsOnAffinityThread() const { return task_runner_->RunsTasksOnCurrentThread(); }

  // 任意线程调用, 幂等。见文件头注释 2)。
  void DeleteLater();
  bool is_delete_scheduled() const { return delete_scheduled_.load(std::memory_order_acquire); }

 protected:
  // 让此前取得的 WeakPtr 全部失效(之后取得的仍有效)。仅归属线程。
  // 典型用法: 析构函数开头或"关闭"时调用, 拦住之后才到达的回调。
  void InvalidateWeakPtrs();

  // 归属当前线程的 EventLoop(EventLoop::Current(), 必须存在)。
  Object();
  // 显式指定归属(如测试替身的 runner)。
  explicit Object(std::shared_ptr<TaskRunner> affinity);

 private:
  template <typename T>
  friend class WeakPtr;

  // 仅在归属线程读写。
  struct Liveness {
    bool alive = true;
  };

  std::shared_ptr<TaskRunner> task_runner_;
  std::shared_ptr<Liveness> liveness_;
  std::atomic<bool> delete_scheduled_{false};

  MOMO_DISALLOW_COPY_AND_ASSIGN(Object);
};

// ObjectPtr 的删除器: 释放即 DeleteLater。
struct ObjectDeleter {
  void operator()(Object* object) const {
    if (object != nullptr) {
      object->DeleteLater();
    }
  }
};

// 独占所有权句柄。可在任意线程释放;析构总在归属线程的事件循环中进行。
template <typename T>
using ObjectPtr = std::unique_ptr<T, ObjectDeleter>;

template <typename T, typename... Args>
ObjectPtr<T> MakeObject(Args&&... args) {
  static_assert(std::is_base_of_v<Object, T>, "MakeObject requires a momo::Object subclass");
  return ObjectPtr<T>(new T(std::forward<Args>(args)...));
}

// 弱引用(QPointer)。可拷贝并随任务投递到任意线程, 但 get() 只能在对象的归属线程调用。
template <typename T>
class WeakPtr {
 public:
  WeakPtr() = default;
  explicit WeakPtr(T* object) {
    static_assert(std::is_base_of_v<Object, T>, "WeakPtr requires a momo::Object subclass");
    if (object != nullptr) {
      const Object* base = object;
      ptr_ = object;
      liveness_ = base->liveness_;
      task_runner_ = base->task_runner_;
    }
  }

  T* get() const {
    if (ptr_ == nullptr) {
      return nullptr;
    }
    MOMO_DCHECK(task_runner_->RunsTasksOnCurrentThread())
        << "WeakPtr dereferenced off the object's affinity thread";
    return liveness_->alive ? ptr_ : nullptr;
  }
  explicit operator bool() const { return get() != nullptr; }
  T* operator->() const { return get(); }
  // 是否曾指向对象(不判活, 任意线程可调用)。
  bool is_bound() const { return ptr_ != nullptr; }

  // 对象归属线程的 runner(对象已析构也可用), 便于"投递过去再判活"。未绑定时为空。
  const std::shared_ptr<TaskRunner>& task_runner() const { return task_runner_; }

 private:
  T* ptr_ = nullptr;
  std::shared_ptr<Object::Liveness> liveness_;
  std::shared_ptr<TaskRunner> task_runner_;
};

// 投递到 weak 所指对象的归属线程;届时对象仍存活才执行 fn(T&)。weak 未绑定时什么也不做。任意线程调用。
template <typename T, typename F>
void PostToObject(const WeakPtr<T>& weak, F&& fn) {
  if (!weak.is_bound()) {
    return;
  }
  weak.task_runner()->PostTask([weak, fn = std::forward<F>(fn)]() mutable {
    if (T* object = weak.get()) {
      fn(*object);
    }
  });
}

}  // namespace momo

// 在 Object 的成员函数中断言当前线程为对象的归属线程(debug 构建)。
#define MOMO_DCHECK_ON_AFFINITY_THREAD() \
  MOMO_DCHECK(IsOnAffinityThread()) << "Called off the object's affinity thread. "
