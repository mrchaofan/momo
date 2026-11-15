#include "engine/shell/engine.h"

#include <utility>

#include "base/logging.h"
#include "engine/extension/extension_host.h"

namespace momo::engine {

Engine::Engine(const Settings& settings, DrawCallback on_draw, std::unique_ptr<VsyncWaiter> waiter,
               const ui::AppHost& host)
    : on_draw_(std::move(on_draw)),
      settings_(settings),
      host_(host),
      animator_(*this, task_runner(), std::move(waiter)) {
  // 帧请求端都在 ui 线程: 纹理新帧由 TextureRegistry 投递到本线程, 扩展的帧回调本来就在本线程注册。
  // 纹理通知可能在 Engine 析构后才执行: 经 WeakPtr 判活。
  if (host_.textures) {
    host_.textures->SetFrameListener(task_runner(), [weak = WeakPtr<Engine>(this)](int64_t) {
      if (Engine* self = weak.get()) self->animator_.RequestFrame();
    });
  }
  if (host_.extensions != nullptr) {
    host_.extensions->SetFrameRequester([this] { animator_.RequestFrame(); });
  }
  UpdateAnimator();
}

Engine::~Engine() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  InvalidateWeakPtrs();
  if (host_.extensions != nullptr) {
    host_.extensions->SetFrameRequester(nullptr);
  }
  if (host_.textures) {
    host_.textures->SetFrameListener(nullptr, nullptr);
  }
  // 运行时先于 animator_ 销毁。
  StopRuntime();
  MOMO_LOG(INFO) << "Engine destroyed.";
}

Status Engine::StartRuntime(uint64_t generation) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_DCHECK(driver_ == nullptr) << "StartRuntime while a runtime exists";
  MOMO_LOG(INFO) << "Starting JS runtime #" << generation << ": "
                 << (settings_.bundle_path.empty() ? "<no bundle>" : settings_.bundle_path.string());
  // 从这里起发出的通道消息属于这一代(MessageChannelHub 的代际)。
  if (host_.hub) {
    host_.hub->SetJsGeneration(generation);
  }
  ui::WindowClient& client = *this;
  auto driver = ui::CreateAppDriver(settings_, client, host_, generation);
  Status status = driver.status();
  if (driver.ok()) {
    driver_ = std::move(driver).value();
  } else {
    // bundle 加载失败(不存在或有错): 退回占位驱动, 引擎保持可用。
    driver_ = std::make_unique<ui::PlaceholderDriver>(client);
  }
  // 运行时创建前就已绑定的视图: 驱动创建时已读到它们, 这里补发各自的状态。
  for (const auto& [view, state] : views_) {
    driver_->OnMetricsChanged(view);
    if (state.focused) {
      driver_->OnFocusChanged(view, true);
    }
    if (!state.visible) {
      driver_->OnVisibilityChanged(view, false);
    }
  }
  animator_.RequestFrame();
  return status;
}

void Engine::StopRuntime() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (driver_ == nullptr) {
    return;
  }
  driver_.reset();  // JS 端点随运行时一起移除(见 services 绑定)
  // 旧运行时已发出、尚在途中的消息从此作废。
  if (host_.hub) {
    host_.hub->SetJsGeneration(0);
  }
  MOMO_LOG(INFO) << "JS runtime stopped.";
}

void Engine::OnAnimatorBeginFrame(TimePoint frame_start, TimePoint) {
  if (driver_ == nullptr) {
    return;  // 没有运行时: 不出帧, 也不执行扩展帧回调(扩展的生命周期监听器不会与帧回调并发)
  }
  // 扩展帧回调先于应用帧(视频/图表类扩展在此推送纹理帧);仍有回调时持续出帧。
  const bool more = host_.extensions != nullptr && host_.extensions->RunFrameCallbacks();
  driver_->BeginFrame(frame_start);
  if (more) {
    animator_.RequestFrame();
  }
}

// ---- 视图 ----

void Engine::AddView(platform::ViewId view) {
  if (views_.count(view) != 0) {
    return;
  }
  views_[view];
  MOMO_LOG(VERBOSE) << "View added: " << view;
  if (driver_) driver_->OnViewAdded(view);
  UpdateAnimator();
}

void Engine::RemoveView(platform::ViewId view) {
  if (views_.erase(view) == 0) {
    return;
  }
  MOMO_LOG(VERBOSE) << "View removed: " << view;
  if (driver_) driver_->OnViewRemoved(view);
  UpdateAnimator();
}

void Engine::Render(std::unique_ptr<graphics::LayerTree> layer_tree) {
  auto it = layer_tree != nullptr ? views_.find(layer_tree->view_id()) : views_.end();
  if (it == views_.end() || !it->second.visible) {
    return;  // 已移除的视图(JS 侧尚未感知)或不可见的视图
  }
  animator_.Render(std::move(layer_tree));
}

platform::ViewportMetrics Engine::metrics(platform::ViewId view) const {
  auto it = views_.find(view);
  return it != views_.end() ? it->second.metrics : platform::ViewportMetrics{};
}

bool Engine::focused(platform::ViewId view) const {
  auto it = views_.find(view);
  return it != views_.end() && it->second.focused;
}

bool Engine::visible(platform::ViewId view) const {
  auto it = views_.find(view);
  return it != views_.end() && it->second.visible;
}

std::vector<platform::ViewId> Engine::views() const {
  std::vector<platform::ViewId> out;
  out.reserve(views_.size());
  for (const auto& [view, state] : views_) {
    out.push_back(view);
  }
  return out;
}

void Engine::OnMetricsChanged(platform::ViewId view, const platform::ViewportMetrics& metrics) {
  auto it = views_.find(view);
  if (it == views_.end() || metrics == it->second.metrics) {
    return;
  }
  it->second.metrics = metrics;
  MOMO_LOG(VERBOSE) << "Metrics (view " << view << "): " << metrics.physical_size.width << "x"
                    << metrics.physical_size.height << " @" << metrics.device_pixel_ratio;
  if (driver_) driver_->OnMetricsChanged(view);
}

void Engine::OnPointerPacket(platform::ViewId view, const platform::PointerDataPacket& packet) {
  if (driver_ == nullptr || packet.empty() || views_.count(view) == 0) {
    return;
  }
  // 指针序列的接收方由宿主(原生捕获)与框架(逻辑捕获)决定, 引擎原样转发。
  driver_->OnPointerPacket(view, packet);
}

void Engine::OnKeyData(platform::ViewId view, const platform::KeyData& data) {
  if (driver_ != nullptr && views_.count(view) != 0) {
    driver_->OnKeyData(view, data);
  }
}

void Engine::OnFocusChanged(platform::ViewId view, bool focused) {
  auto it = views_.find(view);
  if (it == views_.end() || it->second.focused == focused) {
    return;
  }
  it->second.focused = focused;
  if (driver_) driver_->OnFocusChanged(view, focused);
}

void Engine::OnVisibilityChanged(platform::ViewId view, bool visible) {
  auto it = views_.find(view);
  if (it == views_.end() || it->second.visible == visible) {
    return;
  }
  it->second.visible = visible;
  MOMO_LOG(VERBOSE) << "View " << view << (visible ? " visible" : " hidden");
  if (driver_) driver_->OnVisibilityChanged(view, visible);
  UpdateAnimator();
  if (visible) {
    animator_.RequestFrame();  // 重新可见: 补一帧最新画面
  }
}

void Engine::SetAppSuspended(bool suspended) {
  if (app_suspended_ == suspended) {
    return;
  }
  app_suspended_ = suspended;
  MOMO_LOG(INFO) << (suspended ? "App suspended: frames stopped." : "App resumed.");
  UpdateAnimator();
}

void Engine::UpdateAnimator() {
  bool any_visible = false;
  for (const auto& [view, state] : views_) {
    any_visible = any_visible || state.visible;
  }
  if (!app_suspended_ && any_visible) {
    animator_.Start();
  } else {
    animator_.Stop();
  }
}

}  // namespace momo::engine
