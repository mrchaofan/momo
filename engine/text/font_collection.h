#pragma once

// FontCollection: 系统字体(平台字体管理器: Mac CoreText / Win DirectWrite)+ 应用内嵌字体 + 回退链
// (docs/02 §6)。公共头不含 Skia 类型。
// 线程约定: 归属 ui 线程(段落排版在 ui 线程同步进行, docs/05 §6);每个 JS 运行时一个实例。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/macros.h"
#include "base/status.h"

namespace momo::engine::text {

class FontCollection {
 public:
  struct Impl;

  static std::unique_ptr<FontCollection> Create();
  ~FontCollection();

  // 注册内嵌字体(TTF/OTF/TTC 字节)。family 为空时取字体自身的族名。返回注册的族名。
  StatusOr<std::string> RegisterFont(const uint8_t* data, size_t size, const std::string& family);
  // 系统中可用的字体族数量(诊断用)。
  int system_family_count() const;

  Impl& impl() { return *impl_; }

 private:
  explicit FontCollection(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(FontCollection);
};

}  // namespace momo::engine::text
