#include "embedder/common/text_input_channel.h"

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

TextInputChannel::TextInputChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                                   std::shared_ptr<TaskRunner> platform_runner,
                                   ConnectionProvider connection)
    : connection_(std::move(connection)),
      channel_(std::move(hub), std::move(platform_runner), "momo/textinput") {
  using InputFn = std::function<void(TextInputConnection&, const EncodableValue&)>;
  auto input = [this](InputFn fn) -> HostChannel::Handler {
    return [this, fn = std::move(fn)](const EncodableValue& args) -> StatusOr<EncodableValue> {
      if (TextInputConnection* connection = connection_()) {
        fn(*connection, args);
      }
      return EncodableValue();
    };
  };
  channel_.Register("show", input([](TextInputConnection& c, const EncodableValue&) { c.Show(); }));
  channel_.Register("hide", input([](TextInputConnection& c, const EncodableValue&) { c.Hide(); }));
  channel_.Register("setCaretRect", input([](TextInputConnection& c, const EncodableValue& a) {
    c.SetCaretRect(RectArg(a));
  }));
  channel_.Register("setEditingState", input([](TextInputConnection& c, const EncodableValue& a) {
    c.SetEditingState(EditingStateArg(a));
  }));
}

void TextInputChannel::Send(const TextInputEvent& event) {
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
  channel_.Notify(std::move(method), EncodableValue(std::move(args)));
}

}  // namespace momo::embedder
