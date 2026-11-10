#include "engine/graphics/texture.h"

#include <utility>

namespace momo::engine::graphics {

Status Texture::UpdateRgba(const uint8_t* pixels, int width, int height, size_t row_bytes) {
  MOMO_ASSIGN_OR_RETURN(std::shared_ptr<const Image> image,
                        Image::FromRgba(pixels, width, height, row_bytes));
  SetImage(std::move(image));
  return Status::Ok();
}

void Texture::SetImage(std::shared_ptr<const Image> image) {
  MutexLock lock(mutex_);
  latest_ = std::move(image);
}

std::shared_ptr<const Image> Texture::latest() const {
  MutexLock lock(mutex_);
  return latest_;
}

std::shared_ptr<Texture> TextureRegistry::Register() {
  MutexLock lock(mutex_);
  auto texture = std::make_shared<Texture>(next_id_++);
  textures_[texture->id()] = texture;
  return texture;
}

void TextureRegistry::Unregister(int64_t id) {
  MutexLock lock(mutex_);
  textures_.erase(id);  // 仍被 DisplayList/Layer 引用的 Texture 继续存活到它们释放
}

std::shared_ptr<Texture> TextureRegistry::Get(int64_t id) const {
  MutexLock lock(mutex_);
  auto it = textures_.find(id);
  return it == textures_.end() ? nullptr : it->second;
}

void TextureRegistry::MarkFrameAvailable(int64_t id) {
  FrameListener listener;
  std::shared_ptr<TaskRunner> runner;
  {
    MutexLock lock(mutex_);
    if (textures_.count(id) == 0) {
      return;
    }
    listener = listener_;
    runner = listener_runner_;
  }
  if (listener && runner) {
    // 回调在注册方指定的线程执行, 不在扩展的推帧线程上直接调用。
    runner->PostTask([listener = std::move(listener), id] { listener(id); });
  }
}

void TextureRegistry::SetFrameListener(std::shared_ptr<TaskRunner> runner,
                                       FrameListener listener) {
  MutexLock lock(mutex_);
  listener_runner_ = listener ? std::move(runner) : nullptr;
  listener_ = std::move(listener);
}

size_t TextureRegistry::size() const {
  MutexLock lock(mutex_);
  return textures_.size();
}

}  // namespace momo::engine::graphics
