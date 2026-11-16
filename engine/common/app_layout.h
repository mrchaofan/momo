#pragma once

// AppLayout: 一个 Momo 应用在磁盘上的样子(docs/09 §4)。应用路径只有两种:
//   目录  <dir>/package.json 的 "main" 是入口(缺省 index.js);资源在 <dir>/assets/
//   文件  它自己就是入口;资源在它旁边的 assets/
// 打包后的应用是 <resources>/app/ 这个目录, 开发时在命令行给出目录或文件——两者经 LocateApp 走同一条路。
// 这里只定位: 入口文件、assets/ 是否存在不检查(入口不存在是加载时的失败, 由 JS 运行时报告;应用可以没有资源)。

#include <filesystem>

#include "base/status.h"

namespace momo::engine {

struct AppLayout {
  static constexpr const char* kPackageFileName = "package.json";
  static constexpr const char* kDefaultMain = "index.js";
  static constexpr const char* kAssetsDirName = "assets";

  std::filesystem::path entry;       // 入口脚本(绝对路径)
  std::filesystem::path assets_dir;  // 资源目录(绝对路径)
};

// app_path 是目录时读它的 package.json: 读不到、不是合法 JSON、"main" 不是字符串都返回错误。
StatusOr<AppLayout> LocateApp(const std::filesystem::path& app_path);

}  // namespace momo::engine
