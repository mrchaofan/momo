// ext_texture.cc — 纹理注入示例扩展("video-ish", docs/07 §3)。用 C++ 便利封装 momo_extension.hpp 编写
// (纯 C 写法见 hello-extension)。
//
// ext:texture 模块:
//   start(width, height) → textureId   注册外部纹理并开始出帧
//   stop()                             停止出帧并注销纹理
//   frames()                           已推送的帧数
// 机制: 帧回调(ui 线程, 每 vsync)→ 若上一帧已完成, 投递 worker pool 渲染一帧
// (等离子特效, RGBA8)→ UpdatePixels 推送(宿主拷贝并请求新帧)。
// 扩展绝不在 raster 线程被回调;JS 侧用 SceneBuilder.addTexture / Canvas.drawTexture(或框架的
// Texture widget)显示。

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "momo/momo_extension.hpp"

namespace {

using momo::ext::Call;
using momo::ext::Host;
using momo::ext::ModuleBuilder;

class TextureExtension final : public momo::ext::Extension {
 public:
  void OnAttach(Host& host) override {
    host_ = &host;
    host.RegisterModule("ext:texture", [this](ModuleBuilder& m) {
      m.Function("start", [this](Call& call) { Start(call); });
      m.Function("stop", [this](Call& call) {
        Stop();
        call.ReturnNull();
      });
      m.Function("frames", [this](Call& call) { call.Return(static_cast<double>(frames_.load())); });
    });
  }

  // worker 上可能还有一帧在渲染: 宿主在 on_detach 之后排空 worker pool, 之后才 join。
  void OnDetach() override { Stop(); }

 private:
  // ui 线程。
  void Start(Call& call) {
    const auto w = call.Number(0);
    const auto h = call.Number(1);
    if (!w || !h || *w < 1 || *h < 1 || *w > 4096 || *h > 4096) {
      return call.Throw("start(width, height) expects sizes in 1..4096");
    }
    if (running_.load()) {
      return call.Return(static_cast<double>(texture_id_));
    }
    if (rendering_.load()) {
      return call.Throw("previous session is still finishing a frame");
    }
    width_ = static_cast<uint32_t>(*w);
    height_ = static_cast<uint32_t>(*h);
    texture_id_ = host_->RegisterPixelTexture();
    if (texture_id_ == 0) {
      return call.Throw("texture_register failed");
    }
    running_.store(true);
    frame_callback_ = host_->AddFrameCallback([this] { OnFrame(); });
    call.Return(static_cast<double>(texture_id_));
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    host_->RemoveFrameCallback(frame_callback_);
    host_->UnregisterTexture(texture_id_);
    texture_id_ = 0;
  }

  // ui 线程, 每帧: 上一帧渲染完才投递下一帧(背压, 不堆积任务)。
  void OnFrame() {
    if (!running_.load()) return;
    bool expected = false;
    if (rendering_.compare_exchange_strong(expected, true)) {
      if (host_->PostTask(kMomoRunnerWorkerPool, [this] { RenderFrame(); }) != kMomoOk) {
        rendering_.store(false);
      }
    }
  }

  // worker pool。
  void RenderFrame() {
    if (!running_.load()) {
      rendering_.store(false);
      return;
    }
    const double t = static_cast<double>(frames_.load()) / 30.0;
    pixels_.resize(static_cast<size_t>(width_) * height_ * 4);
    for (uint32_t y = 0; y < height_; ++y) {
      for (uint32_t x = 0; x < width_; ++x) {
        const double u = static_cast<double>(x) / width_ * 8.0;
        const double v = static_cast<double>(y) / height_ * 8.0;
        const double p = std::sin(u + t) + std::sin(v * 1.3 - t) + std::sin((u + v + t) * 0.7) +
                         std::sin(std::sqrt(u * u + v * v) + t * 1.7);
        uint8_t* px = &pixels_[(static_cast<size_t>(y) * width_ + x) * 4];
        px[0] = static_cast<uint8_t>(127.5 + 127.5 * std::sin(p * 3.14159));
        px[1] = static_cast<uint8_t>(127.5 + 127.5 * std::sin(p * 3.14159 + 2.094));
        px[2] = static_cast<uint8_t>(127.5 + 127.5 * std::sin(p * 3.14159 + 4.188));
        px[3] = 255;
      }
    }
    host_->UpdatePixels(texture_id_, pixels_.data(), width_, height_,
                        static_cast<size_t>(width_) * 4);
    frames_.fetch_add(1);
    rendering_.store(false);
  }

  Host* host_ = nullptr;
  int64_t texture_id_ = 0;
  uint64_t frame_callback_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::atomic<bool> rendering_{false};
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frames_{0};
  std::vector<uint8_t> pixels_;  // 只被 worker 上的渲染任务访问(rendering_ 保证互斥)
};

}  // namespace

MOMO_EXTENSION(TextureExtension, "momo.example.texture")
