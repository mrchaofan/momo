#include "base/thread.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <utility>

#include "base/logging.h"
#include "base/waitable_event.h"

namespace momo {

struct Thread::Native {
#if defined(_WIN32)
  HANDLE handle = nullptr;
#else
  pthread_t handle{};
#endif
  WaitableEvent* started = nullptr;
};

namespace {

#if defined(_WIN32)
unsigned __stdcall ThreadEntry(void* arg) {
#else
void* ThreadEntry(void* arg) {
#endif
  auto* trampoline = static_cast<std::pair<Thread*, void (Thread::*)()>*>(arg);
  (trampoline->first->*trampoline->second)();
  delete trampoline;
  return 0;
}

}  // namespace

Thread::Thread(std::string name, Options options)
    : name_(std::move(name)), loop_(EventLoop::Create()), native_(std::make_unique<Native>()) {
  task_runner_ = loop_->GetTaskRunner();
  WaitableEvent started;
  native_->started = &started;
  auto* trampoline = new std::pair<Thread*, void (Thread::*)()>(this, &Thread::ThreadMain);
#if defined(_WIN32)
  native_->handle = reinterpret_cast<HANDLE>(
      _beginthreadex(nullptr, static_cast<unsigned>(options.stack_size), &ThreadEntry, trampoline,
                     STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
  MOMO_CHECK(native_->handle != nullptr) << "failed to start thread " << name_;
#else
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (options.stack_size > 0) {
    MOMO_CHECK(pthread_attr_setstacksize(&attr, options.stack_size) == 0)
        << "invalid stack size for thread " << name_;
  }
  const int rc = pthread_create(&native_->handle, &attr, &ThreadEntry, trampoline);
  pthread_attr_destroy(&attr);
  MOMO_CHECK(rc == 0) << "failed to start thread " << name_;
#endif
  started.Wait();
  native_->started = nullptr;
}

void Thread::ThreadMain() {
  SetCurrentThreadName(name_);
  std::shared_ptr<EventLoop> loop = loop_;
  loop->BindToCurrentThread();  // 绑定后 EventLoop::Current() 与 RunsTasksOnCurrentThread 生效
  WaitableEvent* started = native_->started;
  loop->PostTask([started] { started->Signal(); }, Clock::now());
  loop->Run();
}

Thread::~Thread() { Join(); }

void Thread::Join() {
  if (joined_) {
    return;
  }
  MOMO_CHECK(!task_runner_->RunsTasksOnCurrentThread()) << "Thread " << name_ << " joining itself";
  loop_->Terminate();
#if defined(_WIN32)
  WaitForSingleObject(native_->handle, INFINITE);
  CloseHandle(native_->handle);
#else
  pthread_join(native_->handle, nullptr);
#endif
  joined_ = true;
}

void Thread::SetCurrentThreadName(const std::string& name) {
#if defined(_WIN32)
  std::wstring wide(name.begin(), name.end());  // 线程名为 ASCII
  ::SetThreadDescription(::GetCurrentThread(), wide.c_str());
#elif defined(__APPLE__)
  pthread_setname_np(name.c_str());
#else
  pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

}  // namespace momo
