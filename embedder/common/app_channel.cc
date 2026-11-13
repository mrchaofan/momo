#include "embedder/common/app_channel.h"

#include <string>
#include <utility>

namespace momo::embedder {

using engine::EncodableMap;
using engine::EncodableValue;
using namespace channel_args;

namespace {

StatusOr<EncodableValue> Done(Status status) {
  MOMO_RETURN_IF_ERROR(status);
  return EncodableValue();
}

}  // namespace

AppChannel::AppChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                       std::shared_ptr<TaskRunner> platform_runner, Host* host)
    : host_(host), channel_(std::move(hub), std::move(platform_runner), kName) {
  // 退出类方法可能在通道处理器的调用栈里触发关闭(会移除本处理器): Host 负责延后到安全点执行。
  channel_.Register("quit", [this](const EncodableValue&) -> StatusOr<EncodableValue> {
    host_->Quit();
    return EncodableValue();
  });
  channel_.Register("exit", [this](const EncodableValue& a) -> StatusOr<EncodableValue> {
    host_->Exit(static_cast<int>(Int(a, "code").value_or(0)));
    return EncodableValue();
  });
  channel_.Register("hide", [this](const EncodableValue&) { return Done(host_->HideApp()); });
  channel_.Register("show", [this](const EncodableValue&) { return Done(host_->ShowApp()); });
  channel_.Register("focus", [this](const EncodableValue& a) {
    return Done(host_->FocusApp(Bool(a, "steal", false)));
  });
  channel_.Register("getState", [this](const EncodableValue&) -> StatusOr<EncodableValue> {
    return host_->GetAppState();
  });
}

void AppChannel::NotifyState(std::string_view state) {
  channel_.Notify("stateChanged", EncodableValue(EncodableMap{{"state", std::string(state)}}));
}

void AppChannel::NotifyActive(bool active) {
  channel_.Notify("activeChanged", EncodableValue(EncodableMap{{"active", active}}));
}

void AppChannel::NotifyWindowAllClosed() { channel_.Notify("windowAllClosed"); }

void AppChannel::NotifyActivate(bool has_visible_windows) {
  channel_.Notify("activate", EncodableValue(EncodableMap{{"hasVisibleWindows", has_visible_windows}}));
}

void AppChannel::AskBeforeQuit(MoveOnlyFunction<void(bool)> reply) {
  channel_.Ask("beforeQuit", EncodableValue(), std::move(reply));
}

void AppChannel::AskWillQuit(MoveOnlyFunction<void(bool)> reply) {
  channel_.Ask("willQuit", EncodableValue(), std::move(reply));
}

}  // namespace momo::embedder
