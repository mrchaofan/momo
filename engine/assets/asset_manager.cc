#include "engine/assets/asset_manager.h"

#include <fstream>
#include <system_error>

namespace momo::engine::assets {

namespace {

bool IsWithin(const std::filesystem::path& root, const std::filesystem::path& path) {
  auto r = root.begin();
  auto p = path.begin();
  for (; r != root.end(); ++r, ++p) {
    if (r->empty()) {
      continue;  // 末尾的 "/"
    }
    if (p == path.end() || *p != *r) {
      return false;
    }
  }
  return true;
}

}  // namespace

StatusOr<std::filesystem::path> AssetManager::Resolve(std::string_view relative) const {
  if (root_.empty()) {
    return FailedPreconditionError("No assets directory configured");
  }
  if (relative.empty() || relative.find('\0') != std::string_view::npos) {
    return InvalidArgumentError("Invalid asset path");
  }
  const std::filesystem::path rel(relative);
  if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
    return InvalidArgumentError("Asset path must be relative: " + std::string(relative));
  }
  std::error_code ec;
  const std::filesystem::path root = std::filesystem::weakly_canonical(root_, ec);
  if (ec) {
    return NotFoundError("Assets directory not found: " + root_.string());
  }
  // weakly_canonical 会解析符号链接与 "..", 之后再校验仍在根目录内。
  const std::filesystem::path full = std::filesystem::weakly_canonical(root / rel, ec);
  if (ec || !IsWithin(root, full)) {
    return InvalidArgumentError("Asset path escapes the assets directory: " +
                                std::string(relative));
  }
  return full;
}

StatusOr<std::vector<uint8_t>> AssetManager::Load(std::string_view relative) const {
  MOMO_ASSIGN_OR_RETURN(std::filesystem::path path, Resolve(relative));
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return NotFoundError("Asset not found: " + std::string(relative));
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return NotFoundError("Cannot open asset: " + std::string(relative));
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) {
    return InternalError("Cannot read asset: " + std::string(relative));
  }
  in.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) {
    return InternalError("Cannot read asset: " + std::string(relative));
  }
  return data;
}

}  // namespace momo::engine::assets
