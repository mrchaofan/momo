#include "embedder/common/asset_resolver.h"

#include <system_error>
#include <utility>

namespace momo::embedder {

namespace {

std::filesystem::path MakeAbsolute(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  return ec ? path : absolute.lexically_normal();
}

}  // namespace

AssetResolver::AssetResolver(std::filesystem::path resources_dir)
    : resources_dir_(std::move(resources_dir)) {}

StatusOr<ResolvedAssets> AssetResolver::Resolve(const AppConfig& config) const {
  ResolvedAssets out;
  std::error_code ec;

  out.bundle_path = MakeAbsolute(config.bundle_path.value_or(resources_dir_ / kBundleFileName));
  if (!std::filesystem::is_regular_file(out.bundle_path, ec)) {
    return NotFoundError("JS bundle not found: " + out.bundle_path.string());
  }

  if (config.assets_dir) {
    out.assets_dir = MakeAbsolute(*config.assets_dir);
    if (!std::filesystem::is_directory(out.assets_dir, ec)) {
      return NotFoundError("Assets directory not found: " + out.assets_dir.string());
    }
  } else {
    // 默认资源目录可以不存在(应用没有资源)。
    out.assets_dir = MakeAbsolute(resources_dir_ / kAssetsDirName);
  }

  out.extensions_dir = MakeAbsolute(resources_dir_ / kExtensionsDirName);
  return out;
}

}  // namespace momo::embedder
