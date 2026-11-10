#include "engine/text/font_collection.h"

#include <utility>

#include "engine/text/text_internal.h"
#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"

#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#endif

namespace momo::engine::text {

namespace {

sk_sp<SkFontMgr> MakeSystemFontManager() {
#if defined(__APPLE__)
  return SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
  return SkFontMgr_New_DirectWrite();
#else
  return nullptr;
#endif
}

}  // namespace

std::unique_ptr<FontCollection> FontCollection::Create() {
  auto impl = std::make_unique<Impl>();
  impl->system = MakeSystemFontManager();
  impl->assets = sk_make_sp<skia::textlayout::TypefaceFontProvider>();
  impl->collection = sk_make_sp<skia::textlayout::FontCollection>();
  impl->collection->setAssetFontManager(impl->assets);
  if (impl->system) {
    impl->collection->setDefaultFontManager(impl->system);
  }
  impl->collection->enableFontFallback();  // 缺字时按码点在系统字体中回退(中文/emoji)
  impl->unicode = SkUnicodes::ICU::Make();
  return std::unique_ptr<FontCollection>(new FontCollection(std::move(impl)));
}

FontCollection::FontCollection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FontCollection::~FontCollection() = default;

StatusOr<std::string> FontCollection::RegisterFont(const uint8_t* data, size_t size,
                                                   const std::string& family) {
  if (data == nullptr || size == 0) {
    return InvalidArgumentError("RegisterFont: empty font data");
  }
  sk_sp<SkFontMgr> mgr = impl_->system;
  if (mgr == nullptr) {
    return UnavailableError("RegisterFont: no font manager on this platform");
  }
  sk_sp<SkTypeface> typeface = mgr->makeFromData(SkData::MakeWithCopy(data, size));
  if (typeface == nullptr) {
    return InvalidArgumentError("RegisterFont: unsupported or corrupt font data");
  }
  std::string name = family;
  if (name.empty()) {
    SkString sk_name;
    typeface->getFamilyName(&sk_name);
    name = sk_name.c_str();
  }
  impl_->assets->registerTypeface(typeface, SkString(name.c_str()));
  impl_->collection->clearCaches();  // 新族名需要重新解析
  return name;
}

int FontCollection::system_family_count() const {
  return impl_->system ? impl_->system->countFamilies() : 0;
}

}  // namespace momo::engine::text
