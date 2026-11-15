#include "embedder/common/platform_channels.h"

#include <string>
#include <utility>
#include <vector>

#include "engine/common/standard_codec.h"

namespace momo::embedder {

using engine::EncodableMap;
using engine::EncodableValue;

namespace {

Rect RectArg(const EncodableValue& args) {
  Rect r;
  if (const auto* list = args.GetIf<engine::EncodableList>(); list != nullptr && list->size() >= 4) {
    r = {(*list)[0].AsDouble().value_or(0), (*list)[1].AsDouble().value_or(0),
         (*list)[2].AsDouble().value_or(0), (*list)[3].AsDouble().value_or(0)};
  } else if (const auto* f = args.GetIf<std::vector<double>>(); f != nullptr && f->size() >= 4) {
    r = {(*f)[0], (*f)[1], (*f)[2], (*f)[3]};
  }
  return r;
}

TextEditingState EditingStateArg(const EncodableValue& args) {
  TextEditingState state;
  auto num = [&args](const char* key, int32_t fallback) {
    const EncodableValue* v = args.Find(key);
    return v != nullptr ? static_cast<int32_t>(v->AsInt().value_or(fallback)) : fallback;
  };
  if (const EncodableValue* text = args.Find("text")) {
    state.text = text->AsString().value_or("");
  }
  state.selection_base = num("selectionBase", 0);
  state.selection_extent = num("selectionExtent", state.selection_base);
  state.composing_start = num("composingStart", -1);
  state.composing_end = num("composingEnd", -1);
  return state;
}

}  // namespace

PlatformChannels::PlatformChannels(std::shared_ptr<engine::MessageChannelHub> hub,
                                   std::shared_ptr<TaskRunner> platform_runner,
                                   PlatformServices* services, TextInputProvider text_input)
    : services_(services),
      text_input_(std::move(text_input)),
      clipboard_(hub, platform_runner, "momo/clipboard"),
      url_(hub, platform_runner, "momo/url"),
      text_input_channel_(hub, platform_runner, "momo/textinput") {
  // 平台服务: 没有 services 时回复 Unavailable。
  using ServiceFn = std::function<StatusOr<EncodableValue>(PlatformServices&, const EncodableValue&)>;
  auto service = [this](ServiceFn fn) -> HostChannel::Handler {
    return [this, fn = std::move(fn)](const EncodableValue& args) -> StatusOr<EncodableValue> {
      if (services_ == nullptr) {
        return UnavailableError("no platform services");
      }
      return fn(*services_, args);
    };
  };
  clipboard_.Register("getText", service([](PlatformServices& s, const EncodableValue&) -> StatusOr<EncodableValue> {
    MOMO_ASSIGN_OR_RETURN(std::string text, s.GetClipboardText());
    return EncodableValue(std::move(text));
  }));
  clipboard_.Register("setText", service([](PlatformServices& s, const EncodableValue& a) -> StatusOr<EncodableValue> {
    MOMO_RETURN_IF_ERROR(s.SetClipboardText(a.AsString().value_or("")));
    return EncodableValue();
  }));
  url_.Register("open", service([](PlatformServices& s, const EncodableValue& a) -> StatusOr<EncodableValue> {
    MOMO_RETURN_IF_ERROR(s.OpenUrl(a.AsString().value_or("")));
    return EncodableValue();
  }));

  // 输入法: 交给获得焦点的窗口;没有时静默忽略。
  using InputFn = std::function<void(TextInputConnection&, const EncodableValue&)>;
  auto input = [this](InputFn fn) -> HostChannel::Handler {
    return [this, fn = std::move(fn)](const EncodableValue& args) -> StatusOr<EncodableValue> {
      if (TextInputConnection* connection = text_input_ ? text_input_() : nullptr) {
        fn(*connection, args);
      }
      return EncodableValue();
    };
  };
  text_input_channel_.Register("show", input([](TextInputConnection& c, const EncodableValue&) { c.Show(); }));
  text_input_channel_.Register("hide", input([](TextInputConnection& c, const EncodableValue&) { c.Hide(); }));
  text_input_channel_.Register("setCaretRect", input([](TextInputConnection& c, const EncodableValue& a) {
    c.SetCaretRect(RectArg(a));
  }));
  text_input_channel_.Register("setEditingState", input([](TextInputConnection& c, const EncodableValue& a) {
    c.SetEditingState(EditingStateArg(a));
  }));
}

void PlatformChannels::SendTextInput(const TextInputEvent& event) {
  using Type = TextInputEvent::Type;
  EncodableMap args;
  std::string method;
  switch (event.type) {
    case Type::kCommit:
      method = "commit";
      args.emplace_back("text", event.text);
      break;
    case Type::kComposing:
      method = "composing";
      args.emplace_back("text", event.text);
      args.emplace_back("cursor", event.cursor);
      break;
    case Type::kComposingEnd:
      method = "composingEnd";
      break;
    case Type::kCommand:
      method = "command";
      args.emplace_back("command", event.command);
      break;
  }
  if (event.replace_start >= 0 && event.replace_end >= event.replace_start) {
    args.emplace_back("replaceStart", event.replace_start);
    args.emplace_back("replaceEnd", event.replace_end);
  }
  text_input_channel_.Notify(std::move(method), EncodableValue(std::move(args)));
}

}  // namespace momo::embedder
