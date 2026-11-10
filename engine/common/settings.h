#pragma once

// Settings: 一个引擎实例的启动设置(由 embedder 填写, 传给 Shell::Create)。纯数据, 供 shell/ui/extension 共用。

#include <filesystem>

namespace momo::engine {

struct Settings {
  std::filesystem::path bundle_path;  // 空 = 不加载 JS(占位帧, 仅 --dev 下允许)
  std::filesystem::path assets_dir;
  std::filesystem::path extensions_dir;
  bool enable_inspector = false;
  bool dev_mode = false;           // --dev: 开发模式(hot restart、momo:services.isDev)
  int inspector_port = 9229;
  bool inspector_wait = false;     // 执行 bundle 前等待调试器连接
  // V8 code cache 目录(冷启动优化;空 = 不缓存)。由 embedder 设为用户缓存目录。
  std::filesystem::path code_cache_dir;
  // V8 ICU 数据(icudtl.dat)。空 = 使用构建期默认(开发树内的预编译包)。
  std::filesystem::path icu_data_path;
  // 显示器刷新率(Hz), 平台无 VsyncSource(或视图未绑定)时回退定时器使用。<= 0 = 未知(按 kDefaultRefreshRate)。
  double refresh_rate = 0;
};

}  // namespace momo::engine
