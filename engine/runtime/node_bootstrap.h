#pragma once

namespace momo::engine::runtime {

// Node 环境的 preload 脚本(函数体, 参数: process, require, codeCacheDir;见 node_bootstrap.cc)。
// 返回宿主对象 { importModule(url, source, baseDir), flushCompileCache() }。
extern const char kNodeBootstrapSource[];

}  // namespace momo::engine::runtime
