#include "embedder/common/platform_channels.h"

#include <string>
#include <utility>

#include "engine/common/standard_codec.h"

namespace momo::embedder {

using engine::DecodeMethodCall;
using engine::EncodableMap;
using engine::EncodableValue;
using engine::EncodeErrorEnvelope;
using engine::EncodeMethodCall;
using engine::EncodeSuccessEnvelope;
using engine::MessageBytes;
using engine::MessageReply;
using engine::MethodCall;

namespace {

constexpr const char* kChannels[] = {"momo/clipboard", "momo/url", "momo/textinput"};

void ReplyTo(MessageReply& reply, StatusOr<EncodableValue> result) {
  if (result.ok()) {
    reply.Send(EncodeSuccessEnvelope(*result));
  } else {
    reply.Send(EncodeErrorEnvelope("error", result.status().message()));
  }
}

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
    : Object(platform_runner),
      hub_(std::move(hub)),
      services_(services),
      text_input_(std::move(text_input)),
      weak_this_(this) {
  using Fn = std::function<StatusOr<EncodableValue>(PlatformChannels&, const MethodCall&)>;
  auto add = [this, &platform_runner](const char* channel, Fn fn) {
    WeakPtr<PlatformChannels> weak = weak_this_;
    hub_->SetNativeHandler(channel, platform_runner,
                           [weak, fn = std::move(fn)](MessageBytes message, MessageReply reply) {
                             StatusOr<MethodCall> call = DecodeMethodCall(message);
                             if (!call.ok()) {
                               ReplyTo(reply, call.status());
                               return;
                             }
                             PlatformChannels* self = weak.get();
                             if (self == nullptr) {
                               ReplyTo(reply, UnavailableError("the host is shutting down"));
                               return;
                             }
                             ReplyTo(reply, fn(*self, *call));
                           });
  };

  add("momo/clipboard", [](PlatformChannels& self, const MethodCall& call) -> StatusOr<EncodableValue> {
    if (self.services_ == nullptr) {
      return UnavailableError("no platform services");
    }
    if (call.method == "getText") {
      MOMO_ASSIGN_OR_RETURN(std::string text, self.services_->GetClipboardText());
      return EncodableValue(std::move(text));
    }
    if (call.method == "setText") {
      MOMO_RETURN_IF_ERROR(self.services_->SetClipboardText(call.args.AsString().value_or("")));
      return EncodableValue();
    }
    return UnimplementedError("momo/clipboard: unknown method " + call.method);
  });

  add("momo/url", [](PlatformChannels& self, const MethodCall& call) -> StatusOr<EncodableValue> {
    if (self.services_ == nullptr) {
      return UnavailableError("no platform services");
    }
    if (call.method == "open") {
      MOMO_RETURN_IF_ERROR(self.services_->OpenUrl(call.args.AsString().value_or("")));
      return EncodableValue();
    }
    return UnimplementedError("momo/url: unknown method " + call.method);
  });

  add("momo/textinput", [](PlatformChannels& self, const MethodCall& call) -> StatusOr<EncodableValue> {
    TextInputConnection* input = self.text_input_ ? self.text_input_() : nullptr;
    if (input == nullptr) {
      return EncodableValue();  // 没有获得焦点的窗口: 静默忽略
    }
    if (call.method == "show") {
      input->Show();
    } else if (call.method == "hide") {
      input->Hide();
    } else if (call.method == "setCaretRect") {
      input->SetCaretRect(RectArg(call.args));
    } else if (call.method == "setEditingState") {
      input->SetEditingState(EditingStateArg(call.args));
    } else {
      return UnimplementedError("momo/textinput: unknown method " + call.method);
    }
    return EncodableValue();
  });
}

PlatformChannels::~PlatformChannels() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  for (const char* channel : kChannels) {
    hub_->SetNativeHandler(channel, nullptr, nullptr);
  }
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
  hub_->SendToJs("momo/textinput", EncodeMethodCall({method, EncodableValue(std::move(args))}));
}

}  // namespace momo::embedder
