// ext_texture.cc — 外部纹理示例("video-ish", docs/07 §3): 插件在自己的线程上生成像素, 引擎合成进画面。
//
//   start(nativeHost, width, height) → textureId   创建外部纹理并开始出帧
//   stop()                                         停止出帧并销毁纹理
//   frames()                                       已推送的帧数
//
// JS 与原生之间是 Node-API;宿主能力表(momo_host.h)只用到纹理的三个函数。出帧线程按自己的节奏
// (约 60 fps)渲染等离子特效并 texture_update_pixels: 宿主拷贝像素并请求引擎出一帧。JS 侧用框架的
// Texture 视图(或 SceneBuilder.addTexture / Canvas.drawTexture)显示这个纹理 id。

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include <node_api.h>

#include "momo/momo_host.h"

namespace {

class Plasma {
 public:
  // JS 线程。成功返回纹理 id, 失败返回 0。
  int64_t Start(const MomoHost* host, uint32_t width, uint32_t height) {
    Stop();
    if (host->texture_create(host, &texture_id_) != kMomoOk) {
      return 0;
    }
    host_ = host;
    running_.store(true);
    thread_ = std::thread([this, width, height] { Run(width, height); });
    return texture_id_;
  }

  // JS 线程(也是运行时销毁时的清理函数): 返回后出帧线程已结束。
  void Stop() {
    if (!thread_.joinable()) return;
    running_.store(false);
    thread_.join();
    host_->texture_destroy(host_, texture_id_);  // 引擎已关闭时返回 kMomoErrClosed: 纹理随引擎一起没了
    texture_id_ = 0;
  }

  uint64_t frames() const { return frames_.load(); }

 private:
  // 出帧线程。
  void Run(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    auto next = std::chrono::steady_clock::now();
    while (running_.load()) {
      const double t = static_cast<double>(frames_.load()) / 30.0;
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const double u = static_cast<double>(x) / width * 8.0;
          const double v = static_cast<double>(y) / height * 8.0;
          const double p = std::sin(u + t) + std::sin(v * 1.3 - t) + std::sin((u + v + t) * 0.7) +
                           std::sin(std::sqrt(u * u + v * v) + t * 1.7);
          uint8_t* px = &pixels[(static_cast<size_t>(y) * width + x) * 4];
          px[0] = static_cast<uint8_t>(127.5 + 127.5 * std::sin(p * 3.14159));
          px[1] = static_cast<uint8_t>(127.5 + 127.5 * std::sin(p * 3.14159 + 2.094));
          px[2] = static_cast<uint8_t>(127.5 + 127.5 * std::sin(p * 3.14159 + 4.188));
          px[3] = 255;
        }
      }
      if (host_->texture_update_pixels(host_, texture_id_, pixels.data(), width, height,
                                       static_cast<size_t>(width) * 4) != kMomoOk) {
        break;  // 引擎已关闭
      }
      frames_.fetch_add(1);
      next += std::chrono::microseconds(16667);
      std::this_thread::sleep_until(next);
    }
  }

  const MomoHost* host_ = nullptr;
  int64_t texture_id_ = 0;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frames_{0};
};

Plasma* Self(napi_env env) {
  void* data = nullptr;
  napi_get_instance_data(env, &data);
  return static_cast<Plasma*>(data);
}

napi_value Start(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  const MomoHost* host = argc < 3 ? nullptr : momo_host_from_napi(env, argv[0]);
  double width = 0, height = 0;
  if (host == nullptr || napi_get_value_double(env, argv[1], &width) != napi_ok ||
      napi_get_value_double(env, argv[2], &height) != napi_ok || width < 1 || height < 1 ||
      width > 4096 || height > 4096) {
    napi_throw_type_error(env, nullptr, "start(nativeHost, width, height) expects sizes in 1..4096");
    return nullptr;
  }
  const int64_t id = Self(env)->Start(host, static_cast<uint32_t>(width),
                                      static_cast<uint32_t>(height));
  if (id == 0) {
    napi_throw_error(env, nullptr, "texture_create failed");
    return nullptr;
  }
  napi_value result;
  napi_create_int64(env, id, &result);
  return result;
}

napi_value Stop(napi_env env, napi_callback_info) {
  Self(env)->Stop();
  return nullptr;
}

napi_value Frames(napi_env env, napi_callback_info) {
  napi_value result;
  napi_create_double(env, static_cast<double>(Self(env)->frames()), &result);
  return result;
}

}  // namespace

NAPI_MODULE_INIT() {
  auto* plasma = new Plasma;
  napi_set_instance_data(
      env, plasma, [](napi_env, void* data, void*) { delete static_cast<Plasma*>(data); }, nullptr);
  // 运行时销毁时停掉出帧线程(此时宿主表已关闭, 见 momo_host.h 的"关闭")。
  napi_add_env_cleanup_hook(env, [](void* data) { static_cast<Plasma*>(data)->Stop(); }, plasma);
  const napi_property_descriptor properties[] = {
      {"start", nullptr, Start, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stop", nullptr, Stop, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"frames", nullptr, Frames, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, 3, properties);
  return exports;
}
