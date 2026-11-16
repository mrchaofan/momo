#pragma once

// TextInputChannel: 宿主的输入法通道 momo/textinput(docs/03 §5)。输入法会话属于获得焦点的窗口, 所以它
// 和窗口在一起, 不和剪贴板这类无状态的平台服务放在一处。
//
//   JS → 宿主   show(), hide(), setCaretRect([x, y, w, h]), setEditingState({...})
//               (交给当前获得焦点的窗口的 TextInputConnection;没有时忽略)
//   宿主 → JS   commit / composing / composingEnd / command(Send)
// 线程约定: 归属 platform 线程(创建、调用与析构);处理器在 platform 线程运行。

#include <functional>
#include <memory>

#include "base/macros.h"
#include "base/task_runner.h"
#include "embedder/common/host_channel.h"
#include "embedder/common/text_input.h"
#include "engine/common/message_channel_hub.h"

namespace momo::embedder {

class TextInputChannel final {
 public:
  // 当前接收输入法会话的连接(获得焦点的窗口;没有窗口时为空)。
  using ConnectionProvider = std::function<TextInputConnection*()>;

  TextInputChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                   std::shared_ptr<TaskRunner> platform_runner, ConnectionProvider connection);

  // 输入法事件与编辑命令 → JS。
  void Send(const TextInputEvent& event);

 private:
  const ConnectionProvider connection_;
  HostChannel channel_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(TextInputChannel);
};

}  // namespace momo::embedder
