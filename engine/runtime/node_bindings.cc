#include "engine/runtime/node_bindings.h"

#include <utility>

#include "base/logging.h"
#include "node.h"
#include "v8.h"

namespace momo::engine::runtime {

NodeBindings::NodeBindings(node::Environment* env, uv_loop_t* loop,
                           std::shared_ptr<TaskRunner> runner)
    : env_(env), loop_(loop), runner_(std::move(runner)) {
  MOMO_CHECK(env_ != nullptr && loop_ != nullptr && runner_ != nullptr);
}

NodeBindings::~NodeBindings() { Stop(); }

void NodeBindings::PrepareEmbedThread() {
  if (prepared_) {
    return;
  }
  prepared_ = true;
  // 保活句柄: 没有它时循环空闲即"不存活", uv_backend_timeout 返回 0, poll 线程会空转。
  dummy_ = new uv_async_t;
  uv_async_init(loop_, dummy_, nullptr);
  // poll 线程先停在信号量上, 等 StartPolling 跑完第一轮 uv 再开始等待。
  uv_sem_init(&embed_sem_, 0);
  uv_thread_options_t options;
  options.flags = UV_THREAD_HAS_STACK_SIZE;
  options.stack_size = 256 * 1024;
  MOMO_CHECK(uv_thread_create_ex(&embed_thread_, &options, &NodeBindings::EmbedThreadRunner, this) ==
             0)
      << "failed to start the uv poll thread";
}

void NodeBindings::StartPolling() {
  if (!prepared_ || polling_) {
    return;
  }
  polling_ = true;
  // 先跑一轮, 让 uv__io_poll 注册全部事件, 再放行 poll 线程(UvRunOnce 末尾 post 信号量)。
  UvRunOnce();
}

void NodeBindings::Stop() {
  if (!prepared_) {
    return;
  }
  prepared_ = false;
  polling_ = false;
  embed_closed_ = true;
  // poll 线程要么停在信号量上, 要么在等 uv 后端: 两处都唤醒。
  uv_sem_post(&embed_sem_);
  uv_async_send(dummy_);
  uv_thread_join(&embed_thread_);
  uv_sem_destroy(&embed_sem_);
  // 句柄在循环下一次运行时完成关闭(FreeEnvironment 会运行循环直到句柄关闭), 回调里释放内存。
  uv_close(reinterpret_cast<uv_handle_t*>(dummy_),
           [](uv_handle_t* handle) { delete reinterpret_cast<uv_async_t*>(handle); });
  dummy_ = nullptr;
  alive_.reset();  // 已投递、尚未执行的 UvRunOnce 作废
}

void NodeBindings::WakeupEmbedThread() {
  if (dummy_ != nullptr) {
    uv_async_send(dummy_);
  }
}

void NodeBindings::SetKeepAlive(bool keep_alive) {
  if (dummy_ == nullptr) {
    return;
  }
  if (keep_alive) {
    uv_ref(reinterpret_cast<uv_handle_t*>(dummy_));
  } else {
    uv_unref(reinterpret_cast<uv_handle_t*>(dummy_));
  }
}

void NodeBindings::UvRunOnce() {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  // 在 Node 的主 context 中处理 uv 事件(回调会调用 JS)。
  v8::Context::Scope context_scope(node::GetMainContext(env_));
  uv_run(loop_, UV_RUN_NOWAIT);
  // 放行 poll 线程: 按 uv 的最新状态(新的定时器/IO)继续等待。
  uv_sem_post(&embed_sem_);
}

void NodeBindings::WakeupMainThread() {
  std::weak_ptr<int> alive = alive_;
  runner_->PostTask([this, alive] {
    if (alive.lock() != nullptr) {
      UvRunOnce();
    }
  });
}

// static
void NodeBindings::EmbedThreadRunner(void* arg) {
  auto* self = static_cast<NodeBindings*>(arg);
  while (true) {
    // 等 ui 线程处理完上一轮 uv 事件。
    uv_sem_wait(&self->embed_sem_);
    if (self->embed_closed_) {
      break;
    }
    // 等 uv 后端有事件(或到达最近定时器的时刻);只等待, 不执行任何回调。
    self->PollEvents();
    if (self->embed_closed_) {
      break;
    }
    // 交给 ui 线程执行。
    self->WakeupMainThread();
  }
}

}  // namespace momo::engine::runtime
