#include "engine/common/platform_view_registry.h"

#include <iterator>
#include <string>

#include "base/logging.h"

namespace momo::engine {

Status PlatformViewRegistry::RegisterFactory(const std::string& type, Factory factory) {
  if (type.empty() || !factory.create || !factory.dispose) {
    return InvalidArgumentError("platform view factory needs a type name, create and dispose");
  }
  if (type.rfind("momo/", 0) == 0) {
    return InvalidArgumentError("platform view type '" + type + "': the momo/ prefix is reserved");
  }
  if (factories_.count(type) != 0) {
    return FailedPreconditionError("platform view type '" + type + "' is already registered");
  }
  factories_.emplace(type, std::move(factory));
  return Status::Ok();
}

void PlatformViewRegistry::UnregisterFactoriesOf(const std::string& owner) {
  for (auto it = factories_.begin(); it != factories_.end();) {
    it = it->second.owner == owner ? factories_.erase(it) : std::next(it);
  }
}

StatusOr<void*> PlatformViewRegistry::Create(int64_t view_id, const std::string& type,
                                              const std::vector<uint8_t>& params) {
  if (view_id <= 0) {
    return InvalidArgumentError("platform view id must be positive");
  }
  if (views_.count(view_id) != 0) {
    return FailedPreconditionError("platform view " + std::to_string(view_id) + " already exists");
  }
  auto it = factories_.find(type);
  if (it == factories_.end()) {
    return NotFoundError("no platform view factory registered for type '" + type + "'");
  }
  Factory factory = it->second;  // 拷贝: create 回调期间表可能被修改
  std::string error;
  void* native = factory.create(view_id, params, &error);
  if (native == nullptr) {
    return InternalError("platform view '" + type + "' create failed" +
                         (error.empty() ? std::string() : ": " + error));
  }
  if (views_.count(view_id) != 0) {
    // create 回调里重入创建了同一 id(不应发生): 立即释放刚创建的视图。
    factory.dispose(view_id, native);
    return FailedPreconditionError("platform view " + std::to_string(view_id) + " already exists");
  }
  views_.emplace(view_id, Entry{View{view_id, type, native}, std::move(factory)});
  return native;
}

void PlatformViewRegistry::DisposeEntry(Entry entry, const BeforeDispose& before) {
  if (before) {
    before(entry.view.id);
  }
  entry.factory.dispose(entry.view.id, entry.view.native);
}

Status PlatformViewRegistry::Dispose(int64_t view_id, const BeforeDispose& before) {
  auto it = views_.find(view_id);
  if (it == views_.end()) {
    return NotFoundError("no platform view " + std::to_string(view_id));
  }
  Entry entry = std::move(it->second);
  views_.erase(it);
  DisposeEntry(std::move(entry), before);
  return Status::Ok();
}

void PlatformViewRegistry::DisposeAll(const BeforeDispose& before) {
  std::map<int64_t, Entry> all;
  all.swap(views_);
  for (auto& [id, entry] : all) {
    DisposeEntry(std::move(entry), before);
  }
}

void PlatformViewRegistry::DisposeViewsOf(const std::string& owner, const BeforeDispose& before) {
  std::vector<Entry> owned;
  for (auto it = views_.begin(); it != views_.end();) {
    if (it->second.factory.owner == owner) {
      owned.push_back(std::move(it->second));
      it = views_.erase(it);
    } else {
      ++it;
    }
  }
  for (Entry& entry : owned) {
    DisposeEntry(std::move(entry), before);
  }
}

void* PlatformViewRegistry::Get(int64_t view_id) const {
  auto it = views_.find(view_id);
  return it != views_.end() ? it->second.view.native : nullptr;
}

std::vector<PlatformViewRegistry::View> PlatformViewRegistry::views() const {
  std::vector<View> out;
  out.reserve(views_.size());
  for (const auto& [id, entry] : views_) {
    out.push_back(entry.view);
  }
  return out;
}

}  // namespace momo::engine
