#pragma once

// AssetResolver: 定位 JS bundle、资源目录(app 包内 / 开发模式命令行指定)与缓存目录。
// resources_dir 与 cache_dir 由平台入口给出(resources: Mac NSBundle resourcePath, Win exe 所在目录;
// cache: 每个应用自己的用户缓存目录, Mac ~/Library/Caches/<bundle id>, Win %LOCALAPPDATA%\<exe 名>),
// 本类不碰平台 API。打包布局见 docs/09 §4: <resources>/{bundle.js, assets/, extensions/}。

#include <filesystem>

#include "base/status.h"
#include "embedder/common/app_config.h"

namespace momo::embedder {

struct ResolvedAssets {
  std::filesystem::path bundle_path;  // 绝对路径
  std::filesystem::path assets_dir;
  std::filesystem::path extensions_dir;
  // V8 code cache 目录(<cache>/v8);入口没有给出缓存目录时为空(不缓存)。
  std::filesystem::path code_cache_dir;
};

class AssetResolver {
 public:
  static constexpr const char* kBundleFileName = "bundle.js";
  static constexpr const char* kAssetsDirName = "assets";
  static constexpr const char* kExtensionsDirName = "extensions";
  static constexpr const char* kCodeCacheDirName = "v8";

  explicit AssetResolver(std::filesystem::path resources_dir, std::filesystem::path cache_dir = {});

  // bundle: 命令行给了 --bundle 就用它, 否则用应用包内的默认 bundle(<resources>/bundle.js)。这里只定位,
  // 不检查它是否存在(加载时才会发现)。显式指定的 assets 目录不存在返回 NotFound。
  StatusOr<ResolvedAssets> Resolve(const AppConfig& config) const;

 private:
  std::filesystem::path resources_dir_;
  std::filesystem::path cache_dir_;
};

}  // namespace momo::embedder
