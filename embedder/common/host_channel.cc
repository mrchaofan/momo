#include "embedder/common/host_channel.h"

#include <utility>
#include <vector>

#include "base/logging.h"

namespace momo::embedder {

using engine::DecodeEnvelope;
using engine::DecodeMethodCall;
using engine::EncodableValue;
using engine::EncodeErrorEnvelope;
using engine::EncodeMethodCall;
using engine::EncodeSuccessEnvelope;
using engine::MessageBytes;
using engine::MessageReply;
using engine::MethodCall;

namespace {

// JS 据此区分"平台不支持该能力"(unimplemented, 可忽略或降级)与真正的错误。
std::string_view ErrorCode(StatusCode code) {
  switch (code) {
    case StatusCode::kUnimplemented: return "unimplemented";
    case StatusCode::kNotFound: return "notFound";
    case StatusCode::kInvalidArgument: return "invalidArgument";
    default: return "error";
  }
}

void ReplyTo(MessageReply& reply, const StatusOr<EncodableValue>& result) {
  if (result.ok()) {
    reply.Send(EncodeSuccessEnvelope(*result));
  } else {
    reply.Send(EncodeErrorEnvelope(ErrorCode(result.status().code()), result.status().message()));
  }
}

StatusOr<EncodableValue> DispatchTo(const std::map<std::string, HostChannel::Handler, std::less<>>& handlers,
                                    const std::string& channel, const MethodCall& call) {
  auto it = handlers.find(call.method);
  if (it == handlers.end()) {
    return UnimplementedError(channel + ": unknown method " + call.method);
  }
  return it->second(call.args);
}

}  // namespace

HostChannel::HostChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                         std::shared_ptr<TaskRunner> platform_runner, std::string name)
    : hub_(std::move(hub)),
      platform_runner_(std::move(platform_runner)),
      name_(std::move(name)),
      handlers_(std::make_shared<Handlers>()) {
  std::weak_ptr<Handlers> weak = handlers_;
  hub_->SetNativeHandler(name_, platform_runner_,
                         [weak, name = name_](MessageBytes message, MessageReply reply) {
                           StatusOr<MethodCall> call = DecodeMethodCall(message);
                           if (!call.ok()) {
                             ReplyTo(reply, call.status());
                             return;
                           }
                           std::shared_ptr<Handlers> handlers = weak.lock();
                           if (handlers == nullptr) {
                             ReplyTo(reply, UnavailableError(name + ": the host is shutting down"));
                             return;
                           }
                           ReplyTo(reply, DispatchTo(*handlers, name, *call));
                         });
}

HostChannel::~HostChannel() {
  MOMO_DCHECK(platform_runner_->RunsTasksOnCurrentThread());
  hub_->SetNativeHandler(name_, nullptr, nullptr);
  handlers_.reset();
}

void HostChannel::Register(std::string method, Handler handler) {
  (*handlers_)[std::move(method)] = std::move(handler);
}

StatusOr<EncodableValue> HostChannel::Dispatch(const MethodCall& call) const {
  return DispatchTo(*handlers_, name_, call);
}

void HostChannel::Notify(std::string method, EncodableValue args) {
  hub_->SendToJs(name_, EncodeMethodCall({std::move(method), std::move(args)}));
}

void HostChannel::Ask(std::string method, EncodableValue args, MoveOnlyFunction<void(bool)> reply) {
  hub_->SendToJs(name_, EncodeMethodCall({method, std::move(args)}), platform_runner_,
                 [reply = std::move(reply), what = name_ + " " + method](
                     std::optional<MessageBytes> bytes) mutable {
                   bool allow = true;  // 无处理器/运行时已销毁/出错: 允许
                   if (bytes.has_value()) {
                     StatusOr<EncodableValue> result = DecodeEnvelope(*bytes);
                     if (result.ok()) {
                       if (const bool* b = result->GetIf<bool>()) allow = *b;
                     } else {
                       MOMO_LOG(WARNING) << what << ": the handler failed: " << result.status();
                     }
                   }
                   reply(allow);
                 });
}

namespace channel_args {

double Num(const EncodableValue& args, std::string_view key, double fallback) {
  const EncodableValue* v = args.Find(key);
  return v != nullptr ? v->AsDouble().value_or(fallback) : fallback;
}

std::optional<double> OptNum(const EncodableValue& args, std::string_view key) {
  const EncodableValue* v = args.Find(key);
  return v != nullptr ? v->AsDouble() : std::nullopt;
}

bool Bool(const EncodableValue& args, std::string_view key, bool fallback) {
  const EncodableValue* v = args.Find(key);
  if (v == nullptr) return fallback;
  if (const bool* b = v->GetIf<bool>()) return *b;
  return fallback;
}

std::string Str(const EncodableValue& args, std::string_view key) {
  const EncodableValue* v = args.Find(key);
  return v != nullptr ? v->AsString().value_or("") : "";
}

std::optional<int64_t> Int(const EncodableValue& args, std::string_view key) {
  const EncodableValue* v = args.Find(key);
  return v != nullptr ? v->AsInt() : std::nullopt;
}

EncodableValue RectValue(const Rect& r) {
  return EncodableValue(std::vector<double>{r.x, r.y, r.width, r.height});
}

EncodableValue OptRectValue(const std::optional<Rect>& r) {
  return r.has_value() ? RectValue(*r) : EncodableValue();
}

}  // namespace channel_args

}  // namespace momo::embedder
