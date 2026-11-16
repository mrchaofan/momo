#include "embedder/common/platform_channels.h"

#include <string>
#include <utility>

#include "engine/common/standard_codec.h"

namespace momo::embedder {

using engine::EncodableValue;

PlatformChannels::PlatformChannels(std::shared_ptr<engine::MessageChannelHub> hub,
                                   std::shared_ptr<TaskRunner> platform_runner,
                                   PlatformServices& services)
    : clipboard_(hub, platform_runner, "momo/clipboard"), url_(hub, platform_runner, "momo/url") {
  clipboard_.Register("getText", [&services](const EncodableValue&) -> StatusOr<EncodableValue> {
    MOMO_ASSIGN_OR_RETURN(std::string text, services.GetClipboardText());
    return EncodableValue(std::move(text));
  });
  clipboard_.Register("setText", [&services](const EncodableValue& a) -> StatusOr<EncodableValue> {
    MOMO_RETURN_IF_ERROR(services.SetClipboardText(a.AsString().value_or("")));
    return EncodableValue();
  });
  url_.Register("open", [&services](const EncodableValue& a) -> StatusOr<EncodableValue> {
    MOMO_RETURN_IF_ERROR(services.OpenUrl(a.AsString().value_or("")));
    return EncodableValue();
  });
}

}  // namespace momo::embedder
