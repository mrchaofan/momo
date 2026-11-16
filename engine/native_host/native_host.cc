#include "engine/native_host/native_host.h"

#include <utility>
#include <vector>

#include "base/lock.h"
#include "base/logging.h"

namespace momo::engine {

// MomoHost 是首成员: 插件手里的 const MomoHost* 就是 Table*。
struct NativeHost::Table {
  MomoHost api{};
  // 以下三项在 Close 时清空: 表活到进程结束, 但不该把引擎的对象也留到那时。
  Mutex mutex;
  std::shared_ptr<TaskRunner> platform_runner;
  std::shared_ptr<graphics::TextureRegistry> textures;
  std::shared_ptr<PlatformViewRegistry> platform_views;
};

namespace {

using Table = NativeHost::Table;

// 取表里的一项;已关闭为空。
template <typename T>
std::shared_ptr<T> Use(const MomoHost* host, std::shared_ptr<T> Table::*part) {
  auto* table = reinterpret_cast<Table*>(const_cast<MomoHost*>(host));
  if (table == nullptr) return nullptr;
  MutexLock lock(table->mutex);
  return table->*part;
}

MomoStatus PostPlatformTask(const MomoHost* host, MomoTaskFn fn, void* user_data) {
  std::shared_ptr<TaskRunner> runner = Use(host, &Table::platform_runner);
  if (runner == nullptr) return kMomoErrClosed;
  if (fn == nullptr) return kMomoErrInvalidArgs;
  // Close 与任务都在 platform 线程: 关闭之后任务不再执行。
  runner->PostTask([host, fn, user_data] {
    if (Use(host, &Table::platform_runner) != nullptr) {
      fn(user_data);
    }
  });
  return kMomoOk;
}

MomoStatus TextureCreate(const MomoHost* host, int64_t* out_id) {
  std::shared_ptr<graphics::TextureRegistry> textures = Use(host, &Table::textures);
  if (textures == nullptr) return kMomoErrClosed;
  if (out_id == nullptr) return kMomoErrInvalidArgs;
  *out_id = textures->Register()->id();
  return kMomoOk;
}

MomoStatus TextureUpdatePixels(const MomoHost* host, int64_t id, const uint8_t* rgba,
                               uint32_t width, uint32_t height, size_t row_bytes) {
  std::shared_ptr<graphics::TextureRegistry> textures = Use(host, &Table::textures);
  if (textures == nullptr) return kMomoErrClosed;
  std::shared_ptr<graphics::Texture> texture = textures->Get(id);
  if (texture == nullptr) return kMomoErrNotFound;
  if (rgba == nullptr || width == 0 || height == 0 ||
      !texture->UpdateRgba(rgba, static_cast<int>(width), static_cast<int>(height), row_bytes)
           .ok()) {
    return kMomoErrInvalidArgs;
  }
  textures->MarkFrameAvailable(id);
  return kMomoOk;
}

MomoStatus TextureDestroy(const MomoHost* host, int64_t id) {
  std::shared_ptr<graphics::TextureRegistry> textures = Use(host, &Table::textures);
  if (textures == nullptr) return kMomoErrClosed;
  if (textures->Get(id) == nullptr) return kMomoErrNotFound;
  textures->Unregister(id);
  return kMomoOk;
}

MomoStatus PlatformViewRegisterFactory(const MomoHost* host, const char* view_type,
                                       const MomoPlatformViewFactory* factory) {
  std::shared_ptr<PlatformViewRegistry> views = Use(host, &Table::platform_views);
  if (views == nullptr) return kMomoErrClosed;
  if (view_type == nullptr || factory == nullptr ||
      factory->struct_size < sizeof(MomoPlatformViewFactory) || factory->create == nullptr ||
      factory->dispose == nullptr) {
    return kMomoErrInvalidArgs;
  }
  const MomoPlatformViewFactory copy = *factory;
  PlatformViewRegistry::Factory entry;
  entry.create = [copy](int64_t view_id, const std::vector<uint8_t>& params) -> void* {
    return copy.create(copy.user_data, view_id, params.empty() ? nullptr : params.data(),
                       params.size());
  };
  entry.dispose = [copy](int64_t view_id, void* native_view) {
    copy.dispose(copy.user_data, view_id, native_view);
  };
  Status status = views->RegisterFactory(view_type, std::move(entry));
  if (!status.ok()) {
    MOMO_LOG(ERROR) << "platform_view_register_factory: " << status;
    return kMomoErrInvalidArgs;
  }
  return kMomoOk;
}

}  // namespace

NativeHost::NativeHost(std::shared_ptr<TaskRunner> platform_runner,
                             std::shared_ptr<graphics::TextureRegistry> textures,
                             std::shared_ptr<PlatformViewRegistry> platform_views)
    : table_(new Table) {
  MOMO_CHECK(platform_runner != nullptr && textures != nullptr && platform_views != nullptr);
  table_->platform_runner = std::move(platform_runner);
  table_->textures = std::move(textures);
  table_->platform_views = std::move(platform_views);
  MomoHost& api = table_->api;
  api.struct_size = sizeof(MomoHost);
  api.api_version = MOMO_HOST_API_VERSION;
  api.post_platform_task = &PostPlatformTask;
  api.texture_create = &TextureCreate;
  api.texture_update_pixels = &TextureUpdatePixels;
  api.texture_destroy = &TextureDestroy;
  api.platform_view_register_factory = &PlatformViewRegisterFactory;
}

NativeHost::~NativeHost() { Close(); }

const MomoHost* NativeHost::table() const { return &table_->api; }

void NativeHost::Close() {
  std::shared_ptr<TaskRunner> runner;
  std::shared_ptr<graphics::TextureRegistry> textures;
  std::shared_ptr<PlatformViewRegistry> views;
  {
    MutexLock lock(table_->mutex);
    runner.swap(table_->platform_runner);
    textures.swap(table_->textures);
    views.swap(table_->platform_views);
  }
}

}  // namespace momo::engine
