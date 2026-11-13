#pragma once

// AppDriver: 驱动帧内容的"应用侧"(docs/02 §2)。Engine 只面向本接口, 不关心是否链接了 V8。
//  - JS 驱动(engine/ui/js_app_driver.cc, 有 V8 时链接): 运行 bundle, 帧/metrics/关闭请求交给 JS window。
//  - 占位驱动(PlaceholderDriver): 无 bundle(--dev)或无 V8(sanitizer 构建)时, 让帧管线照常运转。
// 不含 V8 类型。线程约定: 仅 ui 线程。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/status.h"
#include "base/task_runner.h"
#include "base/time.h"
#include "engine/common/message_channel_hub.h"
#include "engine/common/settings.h"
#include "engine/graphics/layer_tree.h"
#include "engine/platform/input_types.h"
#include "engine/platform/view.h"
#include "engine/platform/viewport_metrics.h"

namespace momo::engine {
class ExtensionHost;
namespace graphics {
class TextureRegistry;
}
}

namespace momo::engine::ui {

using platform::ViewId;

// 驱动(及 JS window/view 对象)背后的宿主, 由 Engine 实现。多视图: 视图相关的操作都带 ViewId
// (视图 0 = 隐式视图 platform::kImplicitViewId)。
// 只含引擎职责: 帧调度、提交画面、视图的尺寸/焦点。指针序列归谁接收(捕获)由宿主与框架决定,
// 引擎只按视图转发输入。窗口的外观与行为(标题、位置、光标、
// 关闭…)不在这里——它们属于宿主, 经宿主注册的 momo/window 通道直接在 JS 与宿主之间往返。
class WindowClient {
 public:
  virtual void ScheduleFrame() = 0;                                          // window.scheduleFrame()
  // view.render(scene): layer_tree->view_id() 已设置。
  virtual void Render(std::unique_ptr<graphics::LayerTree> layer_tree) = 0;
  virtual platform::ViewportMetrics metrics(ViewId view) const = 0;
  virtual bool focused(ViewId view) const = 0;
  // 视图是否可见(窗口隐藏/最小化时为 false: 不出帧, 提交的画面被丢弃)。
  virtual bool visible(ViewId view) const { (void)view; return true; }
  // 当前存在的视图(升序;可能为空: 全部窗口都已关闭)。
  virtual std::vector<ViewId> views() const { return {platform::kImplicitViewId}; }

 protected:
  ~WindowClient() = default;
};

class AppDriver {
 public:
  virtual ~AppDriver() = default;
  // client.metrics(view) 已更新。
  virtual void OnMetricsChanged(ViewId view) = 0;
  // 一帧: 期间对各视图调用 client.Render 提交本帧。
  virtual void BeginFrame(TimePoint frame_start) = 0;
  virtual void OnPointerPacket(ViewId, const platform::PointerDataPacket&) {}
  // 返回 true 表示应用已消费。
  virtual bool OnKeyData(ViewId, const platform::KeyData&) { return false; }
  virtual void OnFocusChanged(ViewId, bool) {}
  virtual void OnVisibilityChanged(ViewId, bool) {}
  // 视图加入/移除(视图 0 在驱动创建前就存在, 不会通知加入)。
  virtual void OnViewAdded(ViewId) {}
  virtual void OnViewRemoved(ViewId) {}
};

// 驱动可用的引擎设施(ui 线程创建驱动时传入;指针成员非拥有, 须比驱动活得久)。
struct AppHost {
  std::shared_ptr<TaskRunner> ui_runner;
  std::shared_ptr<TaskRunner> io_runner;      // 资源读取/图片解码
  std::shared_ptr<MessageChannelHub> hub;     // momo:services
  ExtensionHost* extensions = nullptr;        // 扩展宿主(ext:* 模块、帧回调;可空)
  std::shared_ptr<graphics::TextureRegistry> textures;  // 外部纹理(可空)
  // JS 调用 process.exit(code): 交给宿主做有序退出(ui 线程调用, 实现方自行投递;可空 = 只停止 JS)。
  std::function<void(int exit_code)> request_exit;
};

// 尺寸变化即请求一帧, 每帧为每个视图提交一棵空 LayerTree(只清屏)。
class PlaceholderDriver final : public AppDriver {
 public:
  explicit PlaceholderDriver(WindowClient& client) : client_(client) {}
  void OnMetricsChanged(ViewId) override { client_.ScheduleFrame(); }
  void BeginFrame(TimePoint) override {
    for (ViewId view : client_.views()) {
      if (!client_.visible(view)) continue;
      const platform::ViewportMetrics m = client_.metrics(view);
      if (!m.physical_size.IsEmpty()) {
        auto tree = std::make_unique<graphics::LayerTree>(m.physical_size, m.device_pixel_ratio);
        tree->set_view_id(view);
        client_.Render(std::move(tree));
      }
    }
  }

 private:
  WindowClient& client_;
};

// 按 settings 创建驱动(bundle 为空时为占位驱动)。bundle 加载/执行失败返回错误(消息含 JS 栈)。
// 实现由链接决定: 有 V8 为 js_app_driver.cc, 否则为 shell/app_driver_no_js.cc。
StatusOr<std::unique_ptr<AppDriver>> CreateAppDriver(const Settings& settings,
                                                     WindowClient& client, const AppHost& host);

}  // namespace momo::engine::ui
