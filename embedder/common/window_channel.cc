#include "embedder/common/window_channel.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/logging.h"

namespace momo::embedder {

using engine::EncodableList;
using engine::EncodableMap;
using engine::EncodableValue;
using engine::MethodCall;
using namespace channel_args;

namespace {

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
      {kCapabilityVisibility, "visibility"},
      {kCapabilityMinMax, "minMax"},
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
  o.show = Bool(a, "show", true);
  return o;
}

// setResizeBorder: reset 为 true 时各边先恢复默认值;再由给出的边覆盖(负数按 0), 未给出的边保持不变。
WindowResizeOptions MergeResizeBorder(WindowResizeOptions o, const EncodableValue& a) {
  if (Bool(a, "reset", false)) {
    const WindowResizeOptions defaults;
    o.border_left = defaults.border_left;
    o.border_top = defaults.border_top;
    o.border_right = defaults.border_right;
    o.border_bottom = defaults.border_bottom;
  }
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

StatusOr<EncodableValue> Done(Status status) {
  MOMO_RETURN_IF_ERROR(status);
  return EncodableValue();
}

}  // namespace

WindowChannel::WindowChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                             std::shared_ptr<TaskRunner> platform_runner, Host* host)
    : host_(host), channel_(std::move(hub), std::move(platform_runner), kName) {
  RegisterMethods();
}

EncodableValue WindowChannel::StateValue(const WindowStateFlags& s) {
  return EncodableValue(EncodableMap{{"visible", s.visible},
                                     {"minimized", s.minimized},
                                     {"maximized", s.maximized},
                                     {"fullscreen", s.fullscreen}});
}

void WindowChannel::RegisterWindowMethod(std::string method, WindowHandler handler) {
  channel_.Register(method, [this, method, handler = std::move(handler)](const EncodableValue& a)
                                -> StatusOr<EncodableValue> {
    const std::optional<int64_t> view = Int(a, "view");
    if (!view.has_value()) {
      return InvalidArgumentError(std::string(kName) + " " + method + ": missing 'view'");
    }
    PlatformWindow* w = host_->FindWindow(*view);
    if (w == nullptr) {
      return NotFoundError(std::string(kName) + " " + method + ": no window for view " +
                           std::to_string(*view));
    }
    return handler(*w, a);
  });
}

void WindowChannel::RegisterMethods() {
  // ---- 窗口的创建与关闭 ----
  channel_.Register("create", [this](const EncodableValue& a) -> StatusOr<EncodableValue> {
    const WindowOptions options = ParseWindowOptions(a);
    if (options.size.IsEmpty()) {
      return InvalidArgumentError("momo/window create: width and height must be positive");
    }
    MOMO_ASSIGN_OR_RETURN(platform::ViewId id, host_->CreateWindow(options));
    return EncodableValue(static_cast<int64_t>(id));
  });
  channel_.Register("close", [this](const EncodableValue& a) -> StatusOr<EncodableValue> {
    const std::optional<int64_t> view = Int(a, "view");
    if (!view.has_value()) return InvalidArgumentError("momo/window close: missing 'view'");
    host_->CloseWindow(*view);  // 未知(已关闭)视图: 空操作
    return EncodableValue();
  });
  channel_.Register("requestClose", [this](const EncodableValue& a) -> StatusOr<EncodableValue> {
    const std::optional<int64_t> view = Int(a, "view");
    if (!view.has_value()) return InvalidArgumentError("momo/window requestClose: missing 'view'");
    host_->RequestCloseWindow(*view);
    return EncodableValue();
  });

  // ---- 状态 ----
  RegisterWindowMethod("getState", [](PlatformWindow& w, const EncodableValue&) -> StatusOr<EncodableValue> {
    const uint32_t caps = w.capabilities();
    EncodableMap state;
    state.emplace_back("frame", (caps & kCapabilityPosition) != 0 ? RectValue(w.screen_frame())
                                                                  : EncodableValue());
    state.emplace_back("controls", OptRectValue(w.window_controls_rect()));
    state.emplace_back("capabilities", CapabilityNames(caps));
    state.emplace_back("state", StateValue(w.state()));
    return EncodableValue(std::move(state));
  });

  // ---- 显示与状态切换 ----
  RegisterWindowMethod("show", [](PlatformWindow& w, const EncodableValue&) -> StatusOr<EncodableValue> {
    w.Show();
    return EncodableValue();
  });
  RegisterWindowMethod("hide", [](PlatformWindow& w, const EncodableValue&) { return Done(w.Hide()); });
  RegisterWindowMethod("minimize", [](PlatformWindow& w, const EncodableValue&) { return Done(w.Minimize()); });
  RegisterWindowMethod("maximize", [](PlatformWindow& w, const EncodableValue&) { return Done(w.Maximize()); });
  RegisterWindowMethod("restore", [](PlatformWindow& w, const EncodableValue&) { return Done(w.Restore()); });
  RegisterWindowMethod("activate", [](PlatformWindow& w, const EncodableValue&) -> StatusOr<EncodableValue> {
    w.Activate();
    return EncodableValue();
  });

  // ---- 外观 ----
  RegisterWindowMethod("setTitle", [](PlatformWindow& w, const EncodableValue& a) -> StatusOr<EncodableValue> {
    w.SetTitle(Str(a, "title"));
    return EncodableValue();
  });
  RegisterWindowMethod("setMinSize", [](PlatformWindow& w, const EncodableValue& a) -> StatusOr<EncodableValue> {
    w.SetMinSize({Num(a, "width", 0), Num(a, "height", 0)});
    return EncodableValue();
  });
  RegisterWindowMethod("setCursor", [](PlatformWindow& w, const EncodableValue& a) -> StatusOr<EncodableValue> {
    MOMO_ASSIGN_OR_RETURN(SystemCursor cursor, CursorFromName(Str(a, "cursor")));
    w.SetCursor(cursor);
    return EncodableValue();
  });
  RegisterWindowMethod("setAlwaysOnTop", [](PlatformWindow& w, const EncodableValue& a) {
    return Done(w.SetAlwaysOnTop(Bool(a, "on", false)));
  });
  RegisterWindowMethod("setPosition", [](PlatformWindow& w, const EncodableValue& a) {
    return Done(w.SetPosition({Num(a, "x", 0), Num(a, "y", 0)}));
  });
  RegisterWindowMethod("setResizable", [](PlatformWindow& w, const EncodableValue& a) {
    WindowResizeOptions options = w.resize_options();
    options.resizable = Bool(a, "resizable", true);
    return Done(w.SetResizeOptions(options));
  });
  RegisterWindowMethod("setResizeBorder", [](PlatformWindow& w, const EncodableValue& a) {
    return Done(w.SetResizeOptions(MergeResizeBorder(w.resize_options(), a)));
  });
  RegisterWindowMethod("setTitleBar", [](PlatformWindow& w, const EncodableValue& a) {
    TitleBarOptions options;
    options.hidden = Bool(a, "hidden", false);
    options.height = Num(a, "height", 0);
    return Done(w.SetTitleBar(options));
  });

  // ---- 拖动与指针 ----
  RegisterWindowMethod("startDrag", [](PlatformWindow& w, const EncodableValue& a) {
    return Done(w.StartWindowDrag(static_cast<int64_t>(Num(a, "sequenceId", 0))));
  });
  RegisterWindowMethod("startMoveLoop", [](PlatformWindow& w, const EncodableValue& a) {
    return Done(w.StartMoveLoop({Num(a, "x", 0), Num(a, "y", 0)}));
  });
  RegisterWindowMethod("endMoveLoop", [](PlatformWindow& w, const EncodableValue&) -> StatusOr<EncodableValue> {
    w.EndMoveLoop();
    return EncodableValue();
  });
  RegisterWindowMethod("capturePointer", [](PlatformWindow& w, const EncodableValue& a) -> StatusOr<EncodableValue> {
    MOMO_ASSIGN_OR_RETURN(bool captured, w.CapturePointer(static_cast<int64_t>(Num(a, "sequenceId", 0))));
    return EncodableValue(captured);
  });
}

void WindowChannel::NotifyFrameChanged(platform::ViewId view, const Rect& frame) {
  channel_.Notify("frameChanged",
                  EncodableValue(EncodableMap{{"view", static_cast<int64_t>(view)}, {"frame", RectValue(frame)}}));
}

void WindowChannel::NotifyControlsChanged(platform::ViewId view, const std::optional<Rect>& controls) {
  channel_.Notify("controlsChanged", EncodableValue(EncodableMap{{"view", static_cast<int64_t>(view)},
                                                                 {"controls", OptRectValue(controls)}}));
}

void WindowChannel::NotifyStateChanged(platform::ViewId view, const WindowStateFlags& state) {
  channel_.Notify("stateChanged", EncodableValue(EncodableMap{{"view", static_cast<int64_t>(view)},
                                                              {"state", StateValue(state)}}));
}

void WindowChannel::NotifyReadyToShow(platform::ViewId view) {
  channel_.Notify("readyToShow", EncodableValue(EncodableMap{{"view", static_cast<int64_t>(view)}}));
}

void WindowChannel::NotifyFocusChanged(platform::ViewId view, bool focused) {
  channel_.Notify("focusChanged", EncodableValue(EncodableMap{{"view", static_cast<int64_t>(view)},
                                                              {"focused", focused}}));
}

void WindowChannel::NotifyClosed(platform::ViewId view) {
  channel_.Notify("closed", EncodableValue(EncodableMap{{"view", static_cast<int64_t>(view)}}));
}

void WindowChannel::RequestClose(platform::ViewId view, MoveOnlyFunction<void(bool allow)> reply) {
  channel_.Ask("closeRequested", EncodableValue(EncodableMap{{"view", static_cast<int64_t>(view)}}),
               std::move(reply));
}

}  // namespace momo::embedder
