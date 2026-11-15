#pragma once

// PlatformChannels: 宿主提供的平台服务通道(docs/03 §5)。embedder 直接在引擎的消息通道上注册处理器,
// 不经过引擎——新增平台服务只改 embedder(对标 Flutter embedder 注册 flutter/platform、
// flutter/textinput 等系统通道)。每个通道是一个 HostChannel(解码、分发、回复与错误码都在那里)。
//
//   momo/clipboard   JS → 宿主   getText() → string, setText(text)
//   momo/url         JS → 宿主   open(url)
//   momo/textinput   JS → 宿主   show(), hide(), setCaretRect([x, y, w, h]), setEditingState({...})
//                                (交给当前获得焦点的窗口的 TextInputConnection)
//                    宿主 → JS   commit / composing / composingEnd / command(SendTextInput)
// 线程约定: 归属 platform 线程(创建、调用与析构);处理器在 platform 线程运行。

#include <functional>
#include <memory>

#include "base/macros.h"
#include "base/task_runner.h"
#include "embedder/common/host_channel.h"
#include "embedder/common/platform_services.h"
#include "embedder/common/text_input.h"
#include "engine/common/message_channel_hub.h"

namespace momo::embedder {

class PlatformChannels final {
 public:
  // 当前接收输入法会话的连接(获得焦点的窗口;没有时为空)。
  using TextInputProvider = std::function<TextInputConnection*()>;

  // services 可为空(无平台服务: 调用回复错误)。services 必须比本对象活得久。
  PlatformChannels(std::shared_ptr<engine::MessageChannelHub> hub,
                   std::shared_ptr<TaskRunner> platform_runner, PlatformServices* services,
                   TextInputProvider text_input);

  // 输入法事件 → momo/textinput(宿主 → JS)。
  void SendTextInput(const TextInputEvent& event);

 private:
  PlatformServices* const services_;
  const TextInputProvider text_input_;
  HostChannel clipboard_;
  HostChannel url_;
  HostChannel text_input_channel_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(PlatformChannels);
};

}  // namespace momo::embedder
