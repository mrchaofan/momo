#pragma once

// PlatformChannels: 宿主提供的无状态平台服务通道(docs/03 §5)。embedder 直接在引擎的消息通道上注册处理器,
// 不经过引擎——新增平台服务只改 embedder(对标 Flutter embedder 注册 flutter/platform 等系统通道)。
// 每个通道是一个 HostChannel(解码、分发、回复与错误码都在那里)。
//
//   momo/clipboard   JS → 宿主   getText() → string, setText(text)
//   momo/url         JS → 宿主   open(url)
// 线程约定: 归属 platform 线程(创建、调用与析构);处理器在 platform 线程运行。

#include <memory>

#include "base/macros.h"
#include "base/task_runner.h"
#include "embedder/common/host_channel.h"
#include "embedder/common/platform_services.h"
#include "engine/common/message_channel_hub.h"

namespace momo::embedder {

class PlatformChannels final {
 public:
  // services 必须比本对象活得久。
  PlatformChannels(std::shared_ptr<engine::MessageChannelHub> hub,
                   std::shared_ptr<TaskRunner> platform_runner, PlatformServices& services);

 private:
  HostChannel clipboard_;
  HostChannel url_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(PlatformChannels);
};

}  // namespace momo::embedder
