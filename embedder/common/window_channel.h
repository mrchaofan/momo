#pragma once

// WindowChannel: 宿主的窗口管理通道(docs/03 §2)。窗口外观与行为属于宿主, 所以由 embedder 直接在
// 引擎的消息通道上注册处理器, JS 与宿主之间往返, 不经过引擎(对标 Flutter 桌面 embedder 的 windowing
// 通道 / Chromium 的 views::Widget ⇄ NativeWidget)。
//
// momo/window(StandardMethodCodec;逻辑点;view 为视图 id):
//   JS → 宿主:
//     create {title?, width, height, minWidth?, minHeight?, x?, y?, nonActivating?, resizable?} → view
//     close {view}                        关闭(不询问 closeRequested)
//     getState {view} → {frame: [x, y, w, h], controls: [x, y, w, h] | null, capabilities: [string]}
//     setTitle {view, title} / setMinSize {view, width, height} / setCursor {view, cursor}
//     setAlwaysOnTop {view, on} / activate {view} / setPosition {view, x, y}
//     setResizable {view, resizable}     用户能否拖动边框缩放窗口
//     setResizeBorder {view, left?, top?, right?, bottom?}   窗口内侧的系统缩放边框(未给出的边保持不变)
//     setTitleBar {view, hidden, height?}
//     startDrag {view, sequenceId}        以进行中的 momo 按下序列开始系统窗口拖动(业务层决定哪里可拖)
//     startMoveLoop {view, x, y} / endMoveLoop {view}   宿主驱动的窗口跟随指针(x, y = 抓取偏移)
//     capturePointer {view, sequenceId} → bool   进行中的 momo 按下序列改由该窗口接收(序列已结束或不匹配时 false)
//   宿主 → JS:
//     frameChanged {view, frame: [x, y, w, h]}
//     controlsChanged {view, controls: [x, y, w, h] | null}
//     closeRequested {view} → bool        用户点了关闭按钮;false 取消(无处理器或出错视为允许)
// momo/app(宿主 → JS):
//     requestExit → bool                  用户/系统请求退出应用(Cmd+Q 等);false 取消
// 错误回复的 code: "unimplemented"(平台不支持该能力)、"notFound"(视图已关闭)、"invalidArgument"、"error"。
// 能力可经 getState 的 capabilities 查询。
// 线程约定: momo::Object, 归属 platform 线程;处理器在 platform 线程运行, 回复经 hub 投递。

#include <functional>
#include <memory>
#include <optional>

#include "base/closure.h"
#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"
#include "base/task_runner.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/platform_window.h"
#include "embedder/common/window_types.h"
#include "engine/common/message_channel_hub.h"
#include "engine/common/standard_codec.h"
#include "engine/platform/view.h"

namespace momo::embedder {

class WindowChannel final : public Object {
 public:
  // 窗口的拥有者(Application)。platform 线程回调。
  class Host {
   public:
    // 创建并显示窗口、绑定到引擎, 返回新视图 id。
    virtual StatusOr<platform::ViewId> CreateWindow(const WindowOptions& options) = 0;
    // 解绑并销毁窗口(未知 id 忽略)。
    virtual void CloseWindow(platform::ViewId view) = 0;
    virtual PlatformWindow* FindWindow(platform::ViewId view) = 0;

   protected:
    ~Host() = default;
  };

  static constexpr const char kChannel[] = "momo/window";
  static constexpr const char kAppChannel[] = "momo/app";

  // hub 上注册 momo/window 处理器(在 platform_runner 上运行)。host 必须比本对象活得久。
  WindowChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                std::shared_ptr<TaskRunner> platform_runner, Host* host);
  ~WindowChannel() override;  // 移除处理器

  // ---- 宿主 → JS ----
  void NotifyFrameChanged(platform::ViewId view, const Rect& frame);
  void NotifyControlsChanged(platform::ViewId view, const std::optional<Rect>& controls);
  // 询问 JS 是否允许关闭窗口 / 退出应用。reply 在 platform 线程回调(JS 无处理器、出错或运行时已不在
  // 时为 true)。
  void RequestClose(platform::ViewId view, MoveOnlyFunction<void(bool allow)> reply);
  void RequestAppExit(MoveOnlyFunction<void(bool allow)> reply);

  // 处理一次 JS 调用(测试可直接调用)。
  StatusOr<engine::EncodableValue> HandleCall(const engine::MethodCall& call);

 private:
  void AskJs(const char* channel, engine::MethodCall call, MoveOnlyFunction<void(bool)> reply);

  const std::shared_ptr<engine::MessageChannelHub> hub_;
  const std::shared_ptr<TaskRunner> platform_runner_;
  Host* const host_;
  const WeakPtr<WindowChannel> weak_this_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WindowChannel);
};

}  // namespace momo::embedder
