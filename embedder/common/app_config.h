#pragma once

// AppConfig: 启动配置(bundle 路径、窗口参数、--dev)。纯数据 + 命令行解析, 无平台 API。
//
// 支持的参数(均为 --key=value 形式, 未知参数忽略, 以兼容 macOS 的 -psn_/-NS* 等系统参数):
//   --bundle=<path>     JS bundle 路径(缺省: <resources>/bundle.js)
//   --assets=<dir>      资源目录(缺省: <resources>/assets)
//   --title=<text>      窗口标题
//   --width=<px> --height=<px>  初始窗口逻辑尺寸
//   --dev               开发模式: 开 V8 Inspector(默认端口 9229)、bundle 变化时 hot restart、
//                       缺少 bundle 时以空引擎启动而非报错
//   --inspect-port=<n>  Inspector 端口(0 = 系统分配;-1 = 不启动 Inspector)
//   --inspect-brk       执行 bundle 前等待调试器连接(隐含 --dev)

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "base/status.h"
#include "embedder/common/platform_window.h"

namespace momo::embedder {

struct AppConfig {
  std::optional<std::filesystem::path> bundle_path;
  std::optional<std::filesystem::path> assets_dir;
  WindowSpec window;
  bool dev_mode = false;
  int inspector_port = 9229;
  bool inspector_wait = false;

  // args 不含 argv[0]。
  static StatusOr<AppConfig> Parse(std::span<const std::string> args);
};

}  // namespace momo::embedder
