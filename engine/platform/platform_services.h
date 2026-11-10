#pragma once

// 低频平台服务(剪贴板、打开 URL 等)。经 MessageChannelHub 暴露给 JS(docs/03 §5)。
// 线程约定: 仅 platform 线程调用。

#include <string>
#include <string_view>

#include "base/status.h"

namespace momo::engine::platform {

class PlatformServices {
 public:
  virtual ~PlatformServices() = default;
  virtual StatusOr<std::string> GetClipboardText() = 0;
  virtual Status SetClipboardText(std::string_view utf8) = 0;
  virtual Status OpenUrl(std::string_view url) = 0;
};

}  // namespace momo::engine::platform
