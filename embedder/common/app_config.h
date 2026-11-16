#pragma once

// AppConfig: 启动配置(应用路径、窗口参数、--dev)。纯数据 + 命令行解析, 无平台 API。
//
//   Momo [<app>] [--key=value ...]
//
//   <app>               应用路径: 目录(含 package.json)或一个 JS 文件(engine/common/app_layout.h)。
//                       缺省是应用包内的 <resources>/app(打包后的应用)。只认第一个位置参数
// 选项均为 --key=value 形式, 未知选项忽略。单横线开头的是系统参数(macOS 的 -psn_0_123、
// -NSDocumentRevisionsDebugMode YES): 连同紧跟其后的值一起忽略。
//   --title=<text>      窗口标题
//   --width=<px> --height=<px>  初始窗口逻辑尺寸
//   --dev               开发模式: 开 Node inspector(默认端口 9229)
//   --inspect-port=<n>  inspector 端口(0 = 系统分配;-1 = 不启动 inspector)
//   --inspect-brk       执行入口前等待调试器连接(隐含 --dev)
//   --no-startup-window 不创建启动窗口(隐式视图 0): 窗口全部由 JS 打开(Widget.create)

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "base/status.h"
#include "embedder/common/platform_window.h"

namespace momo::embedder {

struct AppConfig {
  std::optional<std::filesystem::path> app_path;
  WindowOptions window;
  // 启动时创建并显示隐式视图 0 的窗口(JS 入口执行前即可见, runApp 渲染到这里)。
  bool startup_window = true;
  bool dev_mode = false;
  int inspector_port = 9229;
  bool inspector_wait = false;

  // args 不含 argv[0]。
  static StatusOr<AppConfig> Parse(std::span<const std::string> args);
};

}  // namespace momo::embedder
