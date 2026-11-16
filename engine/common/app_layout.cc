#include "engine/common/app_layout.h"

#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "engine/common/json.h"

namespace momo::engine {

namespace {

// package.json 的 "main"(UTF-8, 相对应用目录);没有这个字段时为 kDefaultMain。
StatusOr<std::filesystem::path> ReadMain(const std::filesystem::path& package_file) {
  std::ifstream file(package_file, std::ios::binary);
  if (!file) {
    return NotFoundError("Not a Momo app (cannot read " + package_file.string() + ")");
  }
  std::ostringstream text;
  text << file.rdbuf();
  StatusOr<JsonValue> package = ParseJson(text.str());
  if (!package.ok()) {
    return InvalidArgumentError(package_file.string() + ": " +
                                std::string(package.status().message()));
  }
  const JsonValue* main = package->Get("main");
  if (main == nullptr) {
    return std::filesystem::path(AppLayout::kDefaultMain);
  }
  const std::string* value = main->AsString();
  if (value == nullptr || value->empty()) {
    return InvalidArgumentError(package_file.string() + ": \"main\" must be a non-empty string");
  }
  return std::filesystem::path(std::u8string(value->begin(), value->end()));
}

}  // namespace

StatusOr<AppLayout> LocateApp(const std::filesystem::path& app_path) {
  std::error_code ec;
  std::filesystem::path path = std::filesystem::absolute(app_path, ec);
  path = (ec ? app_path : path).lexically_normal();

  // 两种应用路径在这里分叉, 得到(应用目录, 入口)之后合并。
  std::filesystem::path root;
  AppLayout layout;
  if (std::filesystem::is_directory(path, ec)) {
    root = path;
    MOMO_ASSIGN_OR_RETURN(std::filesystem::path main, ReadMain(root / AppLayout::kPackageFileName));
    layout.entry = (root / main).lexically_normal();
  } else {
    root = path.parent_path();
    layout.entry = path;
  }
  layout.assets_dir = root / AppLayout::kAssetsDirName;
  return layout;
}

}  // namespace momo::engine
