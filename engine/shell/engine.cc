#include "engine/shell/engine.h"

#include <filesystem>
#include <system_error>
#include <utility>

#include "base/logging.h"
#include "engine/common/standard_codec.h"
#include "engine/extension/extension_host.h"

namespace momo::engine {

namespace {

Status CheckBundle(const Settings& settings) {
  // Shell 的契约: 指定的 bundle 不存在即 NotFound, 与驱动实现(是否链接 V8)无关。
  std::error_code ec;
  if (!settings.bundle_path.empty() && !std::filesystem::is_regular_file(settings.bundle_path, ec)) {
    return NotFoundError("JS bundle is not a readable file: " + settings.bundle_path.string());
  }
  return Status::Ok();
}

}  // namespace

StatusOr<std::unique_ptr<Engine>> Engine::Create(const Settings& settings, Delegate& delegate,
                                                 std::unique_ptr<VsyncWaiter> waiter,
                                                 const ui::AppHost& host) {
  MOMO_RETURN_IF_ERROR(CheckBundle(settings));
  std::unique_ptr<Engine> engine(new Engine(settings, delegate, std::move(waiter), host));
  MOMO_ASSIGN_OR_RETURN(engine->driver_, ui::CreateAppDriver(settings, *engine, engine->host_));
  return engine;
}

Engine::Engine(const Settings& settings, Delegate& delegate,
               std::unique_ptr<VsyncWaiter> waiter, const ui::AppHost& host)
    : delegate_(delegate),
      settings_(settings),
      host_(host),
      animator_(*this, task_runner(), std::move(waiter)) {
  views_[platform::kImplicitViewId];  // 隐式视图: bundle 执行前即存在(宿主随后绑定其窗口)
}

Engine::~Engine() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  // driver_(JS 运行时)先于 animator_ 销毁;JS 端点随运行时一起移除(见 services 绑定)。
  driver_.reset();
  MOMO_LOG(INFO) << "Engine destroyed.";
}

Status Engine::HotRestart() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_RETURN_IF_ERROR(CheckBundle(settings_));
  MOMO_LOG(INFO) << "Hot restart: reloading " << settings_.bundle_path.string();
  driver_.reset();  // 先销毁旧运行时(释放 momo:* 端点), 再建新的
  auto driver = ui::CreateAppDriver(settings_, *this, host_);
  if (!driver.ok()) {
    // 新 bundle 有错: 退回占位驱动, 保持引擎可用(--dev 下修复后可再次重启)。
    driver_ = std::make_unique<ui::PlaceholderDriver>(static_cast<ui::WindowClient&>(*this));
    driver_->OnMetricsChanged(platform::kImplicitViewId);
    return driver.status();
  }
  driver_ = std::move(driver).value();
  for (const auto& [view, state] : views_) {
    driver_->OnMetricsChanged(view);
    if (state.focused) {
      driver_->OnFocusChanged(view, true);
    }
  }
  animator_.RequestFrame();
  return Status::Ok();
}

void Engine::OnAnimatorBeginFrame(TimePoint frame_start, TimePoint) {
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
  driver_->OnViewAdded(view);
}

void Engine::RemoveView(platform::ViewId view) {
  if (views_.erase(view) == 0) {
    return;
  }
  MOMO_LOG(VERBOSE) << "View removed: " << view;
  driver_->OnViewRemoved(view);
}

void Engine::Render(std::unique_ptr<graphics::LayerTree> layer_tree) {
  if (layer_tree == nullptr || views_.count(layer_tree->view_id()) == 0) {
    return;  // 已移除的视图(JS 侧尚未感知)
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
  driver_->OnMetricsChanged(view);
}

void Engine::OnPointerPacket(platform::ViewId view, const platform::PointerDataPacket& packet) {
  if (packet.empty() || views_.count(view) == 0) {
    return;
  }
  // 指针序列的接收方由宿主(原生捕获)与框架(逻辑捕获)决定, 引擎原样转发。
  driver_->OnPointerPacket(view, packet);
}

void Engine::OnKeyData(platform::ViewId view, const platform::KeyData& data) {
  if (views_.count(view) != 0) {
    driver_->OnKeyData(view, data);
  }
}

void Engine::OnFocusChanged(platform::ViewId view, bool focused) {
  auto it = views_.find(view);
  if (it == views_.end() || it->second.focused == focused) {
    return;
  }
  it->second.focused = focused;
  driver_->OnFocusChanged(view, focused);
}

void Engine::OnLifecycleEvent(LifecycleEvent event) {
  // 先调整帧调度, 再通知应用: resumed 时应用的 scheduleFrame 立即生效。
  if (event == LifecycleEvent::kResumed) {
    animator_.Start();
  } else {
    animator_.Stop();
  }
  MOMO_LOG(INFO) << "Lifecycle: " << LifecycleEventName(event);
  if (host_.hub) {
    host_.hub->SendToJs("momo/lifecycle",
                        EncodeStandard(EncodableValue(std::string(LifecycleEventName(event)))));
  }
}

}  // namespace momo::engine
