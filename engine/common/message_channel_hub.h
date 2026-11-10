#pragma once

// MessageChannelHub: 异步、面向字节的双向消息通道(docs/04 §5, 对标 Flutter BinaryMessenger)。
// 平台服务(docs/03 §5)与扩展(docs/07)共用。
//
//   native handler: SetNativeHandler(channel, runner, handler) —— handler 在指定 runner 上被调用。
//   JS 端点:       SetJsEndpoint(ui_runner, endpoint) —— 由 momo:services 绑定安装, 在 ui 线程分发到
//                   JS 的 setHandler 回调。
//   JS → native:   SendToNative(...)(ui 线程, 由 momo:services.send 调用)
//   native → JS:   SendToJs(...)(任意线程)
// 回复: 每条消息恰好回复一次。handler 可在任意线程调用 reply;若 handler 丢弃 reply 未调用,
// 发送方收到空回复(nullopt)。回复总是投递到发送方指定的 reply_runner 上执行(即使已在该线程;
// 为空则丢弃回复)。
// 无 handler 的通道立即回复 nullopt。
// 线程约定: 所有方法线程安全(内部小临界区), 回调不在锁内执行。
// 生命周期: 必须以 std::make_shared 创建(Shell 创建并共享给 Engine/扩展宿主);投递中的任务持有强引用。
// Shell 关闭时移除 JS 端点与全部 native handler, 之后的消息直接回复 nullopt。

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/closure.h"
#include "base/lock.h"
#include "base/macros.h"
#include "base/task_runner.h"

namespace momo::engine {

using MessageBytes = std::vector<uint8_t>;

// 一次性回复句柄(仅可移动)。未调用即析构 = 回复 nullopt。
class MessageReply {
 public:
  using Callback = MoveOnlyFunction<void(std::optional<MessageBytes>)>;

  MessageReply() = default;
  MessageReply(std::shared_ptr<TaskRunner> runner, Callback callback)
      : runner_(std::move(runner)), callback_(std::move(callback)) {}
  MessageReply(MessageReply&& other) noexcept = default;
  MessageReply& operator=(MessageReply&& other) noexcept;
  ~MessageReply();

  void Send(std::optional<MessageBytes> reply);
  explicit operator bool() const { return static_cast<bool>(callback_); }

 private:
  std::shared_ptr<TaskRunner> runner_;
  Callback callback_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(MessageReply);
};

class MessageChannelHub : public std::enable_shared_from_this<MessageChannelHub> {
 public:
  using NativeHandler = std::function<void(MessageBytes message, MessageReply reply)>;
  using JsEndpoint =
      std::function<void(const std::string& channel, MessageBytes message, MessageReply reply)>;

  MessageChannelHub() = default;

  // handler 为空表示移除。
  void SetNativeHandler(const std::string& channel, std::shared_ptr<TaskRunner> runner,
                        NativeHandler handler);
  bool HasNativeHandler(const std::string& channel) const;
  // endpoint 为空表示移除(JS 运行时销毁时)。之后发往 JS 的消息直接回复 nullopt。
  void SetJsEndpoint(std::shared_ptr<TaskRunner> ui_runner, JsEndpoint endpoint);

  void SendToNative(const std::string& channel, MessageBytes message,
                    std::shared_ptr<TaskRunner> reply_runner, MessageReply::Callback reply);
  void SendToJs(const std::string& channel, MessageBytes message,
                std::shared_ptr<TaskRunner> reply_runner = nullptr,
                MessageReply::Callback reply = nullptr);

 private:
  struct Handler {
    std::shared_ptr<TaskRunner> runner;
    NativeHandler fn;
  };

  mutable Mutex mutex_;
  std::map<std::string, Handler> handlers_;
  std::shared_ptr<TaskRunner> js_runner_;
  JsEndpoint js_endpoint_;
  uint64_t js_generation_ = 0;

  MOMO_DISALLOW_COPY_AND_ASSIGN(MessageChannelHub);
};

}  // namespace momo::engine
