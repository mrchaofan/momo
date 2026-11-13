#pragma once

// 低频平台服务(剪贴板、打开 URL 等)。embedder 的平台抽象: 由 embedder 自己注册的通道处理器
// (platform_channels.h: momo/clipboard、momo/url)调用, engine 不可见(docs/03 §5)。
// 新增服务只改 embedder: 在这里加方法、各平台实现、在 PlatformChannels 注册通道。
// 线程约定: 仅 platform 线程调用。

#include <string>
#include <string_view>

#include "base/status.h"

namespace momo::embedder {

class PlatformServices {
 public:
  virtual ~PlatformServices() = default;
  virtual StatusOr<std::string> GetClipboardText() = 0;
  virtual Status SetClipboardText(std::string_view utf8) = 0;
  virtual Status OpenUrl(std::string_view url) = 0;
};

}  // namespace momo::embedder
