#pragma once

// 外部纹理(docs/07 §3 纹理注入): 扩展在自己的线程生成帧, 引擎合成时采样最新帧。
//   Texture         一个外部纹理: 任意线程 UpdateRgba 推送新帧(拷贝为不可变 Image), raster 线程读取最新帧。
//   TextureRegistry id → Texture 的线程安全表;帧可用时回调监听者(引擎据此请求新帧)。
// DisplayList(drawTexture)与 TextureLayer 直接持有 shared_ptr<Texture>, 绘制时取当时的最新帧,
// 因此含纹理的绘制不进入 SkPicture 缓存(见 display_list.h)。
// 首期支持 CPU 位图帧;GPU 共享句柄(IOSurface / D3D 共享纹理)为后续扩展点。
// 线程约定: 全部方法线程安全。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>

#include "base/lock.h"
#include "base/macros.h"
#include "base/task_runner.h"
#include "engine/graphics/image.h"

namespace momo::engine::graphics {

class Texture {
 public:
  explicit Texture(int64_t id) : id_(id) {}
  int64_t id() const { return id_; }

  // 推送一帧 RGBA8888(非预乘)像素。
  Status UpdateRgba(const uint8_t* pixels, int width, int height, size_t row_bytes);
  void SetImage(std::shared_ptr<const Image> image);
  // 最新帧;尚无帧时为空。
  std::shared_ptr<const Image> latest() const;

 private:
  const int64_t id_;
  mutable Mutex mutex_;
  std::shared_ptr<const Image> latest_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Texture);
};

class TextureRegistry {
 public:
  using FrameListener = std::function<void(int64_t texture_id)>;

  TextureRegistry() = default;

  std::shared_ptr<Texture> Register();
  void Unregister(int64_t id);
  std::shared_ptr<Texture> Get(int64_t id) const;
  // 扩展推送新帧后调用(任意线程): 把通知投递给监听者。
  void MarkFrameAvailable(int64_t id);
  // 监听者在注册方指定的 runner 上执行(通常就是注册方自己的线程), 绝不在调用
  // MarkFrameAvailable 的线程上直接调用。传空 listener 注销。
  void SetFrameListener(std::shared_ptr<TaskRunner> runner, FrameListener listener);
  size_t size() const;

 private:
  mutable Mutex mutex_;
  int64_t next_id_ = 1;
  std::map<int64_t, std::shared_ptr<Texture>> textures_;
  std::shared_ptr<TaskRunner> listener_runner_;
  FrameListener listener_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(TextureRegistry);
};

}  // namespace momo::engine::graphics
