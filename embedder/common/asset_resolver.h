#pragma once

// AssetResolver: 定位 JS bundle 与资源目录(app 包内 / 开发模式命令行指定)。
// resources_dir 由平台入口给出(Mac: NSBundle resourcePath;Win: exe 所在目录), 本类不碰平台 API。
// 打包布局见 docs/09 §4: <resources>/{bundle.js, assets/, extensions/, icudtl.dat}。

#include <filesystem>

#include "base/status.h"
#include "embedder/common/app_config.h"

namespace momo::embedder {

struct ResolvedAssets {
  std::filesystem::path bundle_path;  // 绝对路径
  std::filesystem::path assets_dir;
  std::filesystem::path extensions_dir;
};

class AssetResolver {
 public:
  static constexpr const char* kBundleFileName = "bundle.js";
  static constexpr const char* kAssetsDirName = "assets";
  static constexpr const char* kExtensionsDirName = "extensions";
  static constexpr const char* kIcuDataFileName = "icudtl.dat";

  explicit AssetResolver(std::filesystem::path resources_dir);

  // bundle 不存在返回 NotFound;显式指定的 assets 目录不存在返回 NotFound。
  StatusOr<ResolvedAssets> Resolve(const AppConfig& config) const;

  // 资源目录中的 V8 ICU 数据;不存在时返回空路径(引擎回退到构建期默认)。与 bundle 是否存在无关。
  std::filesystem::path IcuDataPath() const;

  const std::filesystem::path& resources_dir() const { return resources_dir_; }

 private:
  std::filesystem::path resources_dir_;
};

}  // namespace momo::embedder
