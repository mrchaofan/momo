#pragma once

// AssetManager: 应用资源(assets/ 目录)的统一读取入口(docs/02 §6)。根目录来自 embedder 的 AssetResolver
// (Settings::assets_dir)。路径一律相对根目录, 拒绝绝对路径与逃出根目录的 "..",
// 也拒绝经符号链接逃出根目录。
// 线程约定: 无可变状态, 任意线程可调用;读取是阻塞 IO, 应在 io 线程调用。

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "base/status.h"

namespace momo::engine::assets {

class AssetManager {
 public:
  explicit AssetManager(std::filesystem::path root) : root_(std::move(root)) {}

  const std::filesystem::path& root() const { return root_; }

  // 解析为根目录内的绝对路径;越界/非法返回 InvalidArgument/PermissionDenied。
  StatusOr<std::filesystem::path> Resolve(std::string_view relative) const;
  StatusOr<std::vector<uint8_t>> Load(std::string_view relative) const;

 private:
  std::filesystem::path root_;
};

}  // namespace momo::engine::assets
