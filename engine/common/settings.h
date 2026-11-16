#pragma once

// Settings: 一个引擎实例的启动设置(由 embedder 填写, 传给 Shell::Create)。纯数据, 供 shell/ui 共用。

#include <filesystem>

namespace momo::engine {

struct Settings {
  std::filesystem::path bundle_path;  // 空 = 不加载 JS(占位帧, 仅 --dev 下允许)
  std::filesystem::path assets_dir;
  bool enable_inspector = false;
  bool dev_mode = false;           // --dev: 开发模式(momo:services.isDev)
  int inspector_port = 9229;
  bool inspector_wait = false;     // 执行 bundle 前等待调试器连接
  // 运行时自带的 JS 模块(框架、内置扩展): import "momo:<name>" 解析到 <builtin_dir>/<name>/(其 package.json 的
  // main)。空 = 没有(只有引擎自己的原生模块 momo:ui、momo:services)。由 embedder 设为 <resources>/builtin。
  std::filesystem::path builtin_dir;
  // V8 code cache 目录(冷启动优化;空 = 不缓存)。由 embedder 设为用户缓存目录。
  std::filesystem::path code_cache_dir;
  // 显示器刷新率(Hz), 平台无 VsyncSource(或视图未绑定)时回退定时器使用。<= 0 = 未知(按 kDefaultRefreshRate)。
  double refresh_rate = 0;
};

}  // namespace momo::engine
