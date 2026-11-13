#include "embedder/common/window_channel.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/logging.h"

namespace momo::embedder {

using engine::DecodeEnvelope;
using engine::DecodeMethodCall;
using engine::EncodableList;
using engine::EncodableMap;
using engine::EncodableValue;
using engine::EncodeErrorEnvelope;
using engine::EncodeMethodCall;
using engine::EncodeSuccessEnvelope;
using engine::MessageBytes;
using engine::MessageReply;
using engine::MethodCall;

namespace {

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

EncodableValue RectValue(const Rect& r) {
  return EncodableValue(std::vector<double>{r.x, r.y, r.width, r.height});
}

EncodableValue OptRectValue(const std::optional<Rect>& r) {
  return r.has_value() ? RectValue(*r) : EncodableValue();
}

StatusOr<SystemCursor> CursorFromName(std::string_view name) {
  static constexpr std::pair<std::string_view, SystemCursor> kCursors[] = {
      {"basic", SystemCursor::kArrow},
      {"text", SystemCursor::kIBeam},
      {"click", SystemCursor::kPointingHand},
      {"precise", SystemCursor::kCrosshair},
      {"resizeLeftRight", SystemCursor::kResizeLeftRight},
      {"resizeUpDown", SystemCursor::kResizeUpDown},
      {"forbidden", SystemCursor::kNotAllowed},
      {"none", SystemCursor::kHidden},
  };
  for (const auto& [n, c] : kCursors) {
    if (name == n) return c;
  }
  return InvalidArgumentError("unknown cursor '" + std::string(name) + "'");
}

EncodableList CapabilityNames(uint32_t caps) {
  static constexpr std::pair<WindowCapability, const char*> kNames[] = {
      {kCapabilityAlwaysOnTop, "alwaysOnTop"},
      {kCapabilityPosition, "position"},
      {kCapabilityResizeOptions, "resizeOptions"},
      {kCapabilityHiddenTitleBar, "hiddenTitleBar"},
      {kCapabilityWindowDrag, "windowDrag"},
      {kCapabilityMoveLoop, "moveLoop"},
      {kCapabilityPointerCapture, "pointerCapture"},
      {kCapabilityPlatformViews, "platformViews"},
      {kCapabilityNonActivating, "nonActivating"},
  };
  EncodableList out;
  for (const auto& [bit, name] : kNames) {
    if ((caps & bit) != 0) out.emplace_back(name);
  }
  return out;
}

WindowOptions ParseWindowOptions(const EncodableValue& a) {
  WindowOptions o;
  o.title = Str(a, "title");
  o.size = {Num(a, "width", 0), Num(a, "height", 0)};
  o.min_size = {Num(a, "minWidth", o.min_size.width), Num(a, "minHeight", o.min_size.height)};
  const std::optional<double> x = OptNum(a, "x");
  const std::optional<double> y = OptNum(a, "y");
  if (x.has_value() && y.has_value()) o.position = Point{*x, *y};
  o.non_activating = Bool(a, "nonActivating", false);
  o.resizable = Bool(a, "resizable", true);
  return o;
}

// setResizeBorder: 给出的边覆盖当前值(负数按 0), 未给出的边保持不变。
WindowResizeOptions MergeResizeBorder(WindowResizeOptions o, const EncodableValue& a) {
  auto edge = [&a](std::string_view key, double current) {
    const std::optional<double> v = OptNum(a, key);
    return v.has_value() ? (*v > 0 ? *v : 0.0) : current;
  };
  o.border_left = edge("left", o.border_left);
  o.border_top = edge("top", o.border_top);
  o.border_right = edge("right", o.border_right);
  o.border_bottom = edge("bottom", o.border_bottom);
  return o;
}

// 错误码: JS 据此区分"平台不支持该能力"(unimplemented, 可忽略或降级)与真正的错误。
std::string_view ErrorCode(StatusCode code) {
  switch (code) {
    case StatusCode::kUnimplemented: return "unimplemented";
    case StatusCode::kNotFound: return "notFound";
    case StatusCode::kInvalidArgument: return "invalidArgument";
    default: return "error";
  }
}

void ReplyTo(MessageReply& reply, StatusOr<EncodableValue> result) {
  if (result.ok()) {
    reply.Send(EncodeSuccessEnvelope(*result));
  } else {
    reply.Send(EncodeErrorEnvelope(ErrorCode(result.status().code()), result.status().message()));
  }
}

}  // namespace

WindowChannel::WindowChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                             std::shared_ptr<TaskRunner> platform_runner, Host* host)
    : Object(platform_runner),
      hub_(std::move(hub)),
      platform_runner_(std::move(platform_runner)),
      host_(host),
      weak_this_(this) {
  WeakPtr<WindowChannel> weak = weak_this_;
  hub_->SetNativeHandler(kChannel, platform_runner_, [weak](MessageBytes message, MessageReply reply) {
    StatusOr<MethodCall> call = DecodeMethodCall(message);
    if (!call.ok()) {
      ReplyTo(reply, call.status());
      return;
    }
    WindowChannel* self = weak.get();
    if (self == nullptr) {
      ReplyTo(reply, UnavailableError("the window host is shutting down"));
      return;
    }
    ReplyTo(reply, self->HandleCall(*call));
  });
}

WindowChannel::~WindowChannel() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  hub_->SetNativeHandler(kChannel, nullptr, nullptr);
}

StatusOr<EncodableValue> WindowChannel::HandleCall(const MethodCall& call) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  const EncodableValue& a = call.args;
  if (call.method == "create") {
    const WindowOptions options = ParseWindowOptions(a);
    if (options.size.IsEmpty()) {
      return InvalidArgumentError("momo/window create: width and height must be positive");
    }
    MOMO_ASSIGN_OR_RETURN(platform::ViewId id, host_->CreateWindow(options));
    return EncodableValue(static_cast<int64_t>(id));
  }

  const EncodableValue* view_value = a.Find("view");
  const std::optional<int64_t> view = view_value != nullptr ? view_value->AsInt() : std::nullopt;
  if (!view.has_value()) {
    return InvalidArgumentError("momo/window " + call.method + ": missing 'view'");
  }
  if (call.method == "close") {
    host_->CloseWindow(*view);  // 未知(已关闭)视图: 空操作
    return EncodableValue();
  }
  PlatformWindow* w = host_->FindWindow(*view);
  if (w == nullptr) {
    return NotFoundError("momo/window " + call.method + ": no window for view " + std::to_string(*view));
  }

  if (call.method == "getState") {
    const uint32_t caps = w->capabilities();
    EncodableMap state;
    state.emplace_back("frame", (caps & kCapabilityPosition) != 0 ? RectValue(w->screen_frame())
                                                                  : EncodableValue());
    state.emplace_back("controls", OptRectValue(w->window_controls_rect()));
    state.emplace_back("capabilities", CapabilityNames(caps));
    return EncodableValue(std::move(state));
  }
  if (call.method == "setTitle") {
    w->SetTitle(Str(a, "title"));
    return EncodableValue();
  }
  if (call.method == "setMinSize") {
    w->SetMinSize({Num(a, "width", 0), Num(a, "height", 0)});
    return EncodableValue();
  }
  if (call.method == "setCursor") {
    MOMO_ASSIGN_OR_RETURN(SystemCursor cursor, CursorFromName(Str(a, "cursor")));
    w->SetCursor(cursor);
    return EncodableValue();
  }
  if (call.method == "capturePointer") {
    MOMO_ASSIGN_OR_RETURN(bool captured, w->CapturePointer(static_cast<int64_t>(Num(a, "sequenceId", 0))));
    return EncodableValue(captured);
  }
  if (call.method == "activate") {
    w->Activate();
    return EncodableValue();
  }
  Status status = Status::Ok();
  if (call.method == "setAlwaysOnTop") {
    status = w->SetAlwaysOnTop(Bool(a, "on", false));
  } else if (call.method == "setPosition") {
    status = w->SetPosition({Num(a, "x", 0), Num(a, "y", 0)});
  } else if (call.method == "setResizable") {
    WindowResizeOptions options = w->resize_options();
    options.resizable = Bool(a, "resizable", true);
    status = w->SetResizeOptions(options);
  } else if (call.method == "setResizeBorder") {
    status = w->SetResizeOptions(MergeResizeBorder(w->resize_options(), a));
  } else if (call.method == "setTitleBar") {
    TitleBarOptions options;
    options.hidden = Bool(a, "hidden", false);
    options.height = Num(a, "height", 0);
    status = w->SetTitleBar(options);
  } else if (call.method == "startDrag") {
    status = w->StartWindowDrag(static_cast<int64_t>(Num(a, "sequenceId", 0)));
  } else if (call.method == "startMoveLoop") {
    status = w->StartMoveLoop({Num(a, "x", 0), Num(a, "y", 0)});
  } else if (call.method == "endMoveLoop") {
    w->EndMoveLoop();
  } else {
    return UnimplementedError("momo/window: unknown method " + call.method);
  }
  MOMO_RETURN_IF_ERROR(status);
  return EncodableValue();
}

void WindowChannel::NotifyFrameChanged(platform::ViewId view, const Rect& frame) {
  EncodableMap args{{"view", static_cast<int64_t>(view)}, {"frame", RectValue(frame)}};
  hub_->SendToJs(kChannel, EncodeMethodCall({"frameChanged", EncodableValue(std::move(args))}));
}

void WindowChannel::NotifyControlsChanged(platform::ViewId view,
                                          const std::optional<Rect>& controls) {
  EncodableMap args{{"view", static_cast<int64_t>(view)}, {"controls", OptRectValue(controls)}};
  hub_->SendToJs(kChannel, EncodeMethodCall({"controlsChanged", EncodableValue(std::move(args))}));
}

void WindowChannel::RequestClose(platform::ViewId view, MoveOnlyFunction<void(bool allow)> reply) {
  EncodableMap args{{"view", static_cast<int64_t>(view)}};
  AskJs(kChannel, {"closeRequested", EncodableValue(std::move(args))}, std::move(reply));
}

void WindowChannel::RequestAppExit(MoveOnlyFunction<void(bool allow)> reply) {
  AskJs(kAppChannel, {"requestExit", EncodableValue()}, std::move(reply));
}

void WindowChannel::AskJs(const char* channel, MethodCall call,
                          MoveOnlyFunction<void(bool)> reply) {
  // 回复不依赖本对象存活: 期间宿主已关闭时, 调用方的回调自己判活。
  hub_->SendToJs(channel, EncodeMethodCall(call), platform_runner_,
                 [reply = std::move(reply)](std::optional<MessageBytes> bytes) mutable {
                   bool allow = true;  // 无处理器/运行时已销毁/出错: 允许
                   if (bytes.has_value()) {
                     StatusOr<EncodableValue> result = DecodeEnvelope(*bytes);
                     if (result.ok()) {
                       if (const bool* b = result->GetIf<bool>()) allow = *b;
                     } else {
                       MOMO_LOG(WARNING) << "close/exit request handler failed: " << result.status();
                     }
                   }
                   reply(allow);
                 });
}

}  // namespace momo::embedder
