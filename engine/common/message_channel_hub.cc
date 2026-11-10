#include "engine/common/message_channel_hub.h"

#include <utility>

namespace momo::engine {

MessageReply& MessageReply::operator=(MessageReply&& other) noexcept {
  if (this != &other) {
    Send(std::nullopt);  // 被覆盖的回复视为未回复
    runner_ = std::move(other.runner_);
    callback_ = std::move(other.callback_);
  }
  return *this;
}

MessageReply::~MessageReply() { Send(std::nullopt); }

void MessageReply::Send(std::optional<MessageBytes> reply) {
  if (!callback_) {
    return;
  }
  Callback callback = std::move(callback_);
  callback_ = nullptr;
  if (runner_ == nullptr) {
    return;  // 发送方不关心回复
  }
  // 总是投递(即使已在目标线程): 回复可能产生于 JS 调用栈内(如无 handler 时同步回复),
  // 同步执行会在 JS 执行中途重入 JS(排空 microtask)。
  runner_->PostTask([callback = std::move(callback), reply = std::move(reply)]() mutable {
    callback(std::move(reply));
  });
}

void MessageChannelHub::SetNativeHandler(const std::string& channel,
                                         std::shared_ptr<TaskRunner> runner,
                                         NativeHandler handler) {
  MutexLock lock(mutex_);
  if (!handler || runner == nullptr) {
    handlers_.erase(channel);
    return;
  }
  handlers_[channel] = Handler{std::move(runner), std::move(handler)};
}

bool MessageChannelHub::HasNativeHandler(const std::string& channel) const {
  MutexLock lock(mutex_);
  return handlers_.count(channel) != 0;
}

void MessageChannelHub::SetJsEndpoint(std::shared_ptr<TaskRunner> ui_runner, JsEndpoint endpoint) {
  MutexLock lock(mutex_);
  js_generation_++;
  if (!endpoint || ui_runner == nullptr) {
    js_runner_.reset();
    js_endpoint_ = nullptr;
    return;
  }
  js_runner_ = std::move(ui_runner);
  js_endpoint_ = std::move(endpoint);
}

void MessageChannelHub::SendToNative(const std::string& channel, MessageBytes message,
                                     std::shared_ptr<TaskRunner> reply_runner,
                                     MessageReply::Callback reply) {
  Handler handler;
  {
    MutexLock lock(mutex_);
    auto it = handlers_.find(channel);
    if (it != handlers_.end()) {
      handler = it->second;
    }
  }
  MessageReply reply_handle(std::move(reply_runner), std::move(reply));
  if (!handler.fn) {
    return;  // reply_handle 析构 → 回复 nullopt
  }
  handler.runner->PostTask([fn = std::move(handler.fn), message = std::move(message),
                            reply = std::move(reply_handle)]() mutable {
    fn(std::move(message), std::move(reply));
  });
}

void MessageChannelHub::SendToJs(const std::string& channel, MessageBytes message,
                                 std::shared_ptr<TaskRunner> reply_runner,
                                 MessageReply::Callback reply) {
  std::shared_ptr<TaskRunner> runner;
  uint64_t generation = 0;
  {
    MutexLock lock(mutex_);
    runner = js_runner_;
    generation = js_generation_;
  }
  MessageReply reply_handle(std::move(reply_runner), std::move(reply));
  if (runner == nullptr) {
    return;
  }
  // 执行时(ui 线程)再取端点并核对代数: 端点在投递之后被移除/替换(JS 运行时已销毁)则不调用。
  runner->PostTask([self = shared_from_this(), generation, channel, message = std::move(message),
                    reply = std::move(reply_handle)]() mutable {
    JsEndpoint endpoint;
    {
      MutexLock lock(self->mutex_);
      if (generation != self->js_generation_ || !self->js_endpoint_) {
        return;
      }
      endpoint = self->js_endpoint_;
    }
    endpoint(channel, std::move(message), std::move(reply));
  });
}

}  // namespace momo::engine
