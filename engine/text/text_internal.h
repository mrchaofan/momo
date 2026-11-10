#pragma once

// engine/text 内部头(含 Skia 类型), 只被 engine/text 的实现文件包含。

#include "engine/text/font_collection.h"
#include "include/core/SkFontMgr.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"
#include "modules/skunicode/include/SkUnicode_icu.h"

namespace momo::engine::text {

struct FontCollection::Impl {
  sk_sp<SkFontMgr> system;
  sk_sp<skia::textlayout::TypefaceFontProvider> assets;
  sk_sp<skia::textlayout::FontCollection> collection;
  sk_sp<SkUnicode> unicode;
};

}  // namespace momo::engine::text
