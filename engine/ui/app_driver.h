#pragma once

// AppDriver: 驱动帧内容的"应用侧"(docs/02 §2)。Engine 只面向本接口, 不关心是否链接了 V8。
//  - JS 驱动(engine/ui/js_app_driver.cc, 有 V8 时链接): 运行 bundle, 帧/metrics/关闭请求交给 JS window。
//  - 占位驱动(PlaceholderDriver): 无 bundle(--dev)或无 V8(sanitizer 构建)时, 让帧管线照常运转。
// 不含 V8 类型。线程约定: 仅 ui 线程。

#include <cstdint>
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
#include "engine/platform/platform_view.h"
#include "engine/platform/system_cursor.h"
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
// (视图 0 = 主窗口 platform::kImplicitViewId)。
class WindowClient {
 public:
  virtual void ScheduleFrame() = 0;                                          // window.scheduleFrame()
  // view.render(scene): layer_tree->view_id() 已设置。
  virtual void Render(std::unique_ptr<graphics::LayerTree> layer_tree) = 0;
  virtual void SetTitle(ViewId view, std::string title) = 0;                 // view.title = ...
  virtual void SetCursor(ViewId view, platform::SystemCursor cursor) = 0;    // view.setCursor()
  virtual void SetMinSize(ViewId view, Size logical_size) = 0;               // view.setMinSize()
  virtual platform::ViewportMetrics metrics(ViewId view) const = 0;
  // 内容区在屏幕上的框(逻辑点, 主屏左上原点);宿主未报告时为空矩形。
  virtual Rect screen_frame(ViewId view) const {
    (void)view;
    return {};
  }
  // view.setPosition(x, y): 移动窗口, 使内容区左上角位于屏幕位置(逻辑点)。
  virtual void SetWindowPosition(ViewId view, Point top_left) {
    (void)view;
    (void)top_left;
  }
  virtual bool focused(ViewId view) const = 0;
  // 当前存在的视图(升序;至少含视图 0)。
  virtual std::vector<ViewId> views() const { return {platform::kImplicitViewId}; }
  // createView(options): 异步;结果经 AppDriver::OnViewCreateResult(request, ...) 返回。
  virtual void CreateView(uint64_t request, const platform::ViewOptions& options) {
    (void)request;
    (void)options;
  }
  // view.close(): 请求宿主关闭视图(不询问 onCloseRequested)。完成后经 OnViewRemoved 通知。
  virtual void CloseView(ViewId view) { (void)view; }
  // view.setAlwaysOnTop(on): 窗口浮在普通窗口之上(macOS: 浮动窗口层级)。
  virtual void SetAlwaysOnTop(ViewId view, bool on) {
    (void)view;
    (void)on;
  }
  // view.activate(): 把窗口带到最前并成为 key window。
  virtual void ActivateView(ViewId view) { (void)view; }
  // view.setCapture(): 把进行中的鼠标按下序列转交给该窗口(类似 Win32 SetCapture / Chromium
  // Widget::SetCapture)。之后的拖动与抬起都投递给它, 直到抬起;原窗口随后关闭也不影响。
  // 返回是否有进行中的序列被转交(没有按住的按键时为 false)。
  virtual bool SetPointerCapture(ViewId view) {
    (void)view;
    return false;
  }
  // view.dispatchPlatformViewPointer(): 框架对原生指针事件的处置(把它投递给平台视图、平台视图的指针
  // 进入/离开、按下由 momo 消费)。宿主在 platform 线程执行。
  virtual void DispatchPlatformViewPointer(ViewId view, int64_t platform_view_id,
                                           uint64_t native_event_id,
                                           platform::PlatformViewPointerAction action) {
    (void)view;
    (void)platform_view_id;
    (void)native_event_id;
    (void)action;
  }
  // view.setResizeOptions(): 窗口的缩放行为(可否缩放、内侧缩放边框、拖拽缩放时等待新帧的上限)。
  virtual void SetResizeOptions(ViewId view, const platform::WindowResizeOptions& options) {
    (void)view;
    (void)options;
  }
  // view.setTitleBarStyle(): 隐藏系统标题栏(内容铺满窗口, 保留红绿灯);traffic_center_y 为红绿灯
  // 中心距窗口顶部的逻辑像素(<= 0 用系统默认位置)。
  virtual void SetTitleBar(ViewId view, bool hidden, double traffic_center_y) {
    (void)view;
    (void)hidden;
    (void)traffic_center_y;
  }
  // view.setDragRegions(): 可拖动窗口的区域(逻辑像素, 左上原点);在其中按下鼠标拖动窗口, 双击缩放。
  virtual void SetDragRegions(ViewId view, std::vector<Rect> rects) {
    (void)view;
    (void)rects;
  }

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
  // 返回 false 表示取消关闭。
  virtual bool OnCloseRequested(ViewId view) = 0;
  virtual void OnPointerPacket(ViewId, const platform::PointerDataPacket&) {}
  // 返回 true 表示应用已消费。
  virtual bool OnKeyData(ViewId, const platform::KeyData&) { return false; }
  virtual void OnFocusChanged(ViewId, bool) {}
  // client.screen_frame(view) 已更新。
  virtual void OnWindowFrameChanged(ViewId) {}
  // 视图加入/移除(视图 0 在驱动创建前就存在, 不会通知)。
  virtual void OnViewAdded(ViewId) {}
  virtual void OnViewRemoved(ViewId) {}
  // createView 的结果: 成功为新视图 id(OnViewAdded 已先于它到达), 失败为错误。
  virtual void OnViewCreateResult(uint64_t request, StatusOr<ViewId> result) {
    (void)request;
    (void)result;
  }
};

// 驱动可用的引擎设施(ui 线程创建驱动时传入;指针成员非拥有, 须比驱动活得久)。
struct AppHost {
  std::shared_ptr<TaskRunner> ui_runner;
  std::shared_ptr<TaskRunner> io_runner;      // 资源读取/图片解码
  std::shared_ptr<MessageChannelHub> hub;     // momo:services
  ExtensionHost* extensions = nullptr;        // 扩展宿主(ext:* 模块、帧回调;可空)
  std::shared_ptr<graphics::TextureRegistry> textures;  // 外部纹理(可空)
};

// 尺寸变化即请求一帧, 每帧为每个视图提交一棵空 LayerTree(只清屏);关闭一律放行。
class PlaceholderDriver final : public AppDriver {
 public:
  explicit PlaceholderDriver(WindowClient& client) : client_(client) {}
  void OnMetricsChanged(ViewId) override { client_.ScheduleFrame(); }
  void BeginFrame(TimePoint) override {
    for (ViewId view : client_.views()) {
      const platform::ViewportMetrics m = client_.metrics(view);
      if (!m.physical_size.IsEmpty()) {
        auto tree = std::make_unique<graphics::LayerTree>(m.physical_size, m.device_pixel_ratio);
        tree->set_view_id(view);
        client_.Render(std::move(tree));
      }
    }
  }
  bool OnCloseRequested(ViewId) override { return true; }
  void OnViewCreateResult(uint64_t, StatusOr<ViewId>) override {}

 private:
  WindowClient& client_;
};

// 按 settings 创建驱动(bundle 为空时为占位驱动)。bundle 加载/执行失败返回错误(消息含 JS 栈)。
// 实现由链接决定: 有 V8 为 js_app_driver.cc, 否则为 shell/app_driver_no_js.cc。
StatusOr<std::unique_ptr<AppDriver>> CreateAppDriver(const Settings& settings,
                                                     WindowClient& client, const AppHost& host);

}  // namespace momo::engine::ui
