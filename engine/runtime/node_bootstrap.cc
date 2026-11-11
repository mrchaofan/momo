#include "engine/runtime/node_bootstrap.h"

namespace momo::engine::runtime {

// 在 Node 的 preload 阶段运行(早于任何用户代码), require 可访问 Node 内部模块——Node 版本由
// cmake/deps.lock.json 锁定, 升级时需核对这里用到的内部 API(internal/modules/esm/loader 的
// getOrInitializeCascadedLoader)。不用 runEntryPointWithESMLoader: 它把加载/求值错误当作未捕获异常
// 上报并让 Promise 正常结束, 宿主拿不到错误。
//
//   - momo:* / ext:*: 由 module.registerHooks 解析, 模块内容来自 process._linkedBinding(宿主用
//     node::AddLinkedBinding 注册), 导出名在加载时从绑定对象读出。
//   - 宿主给出源码的模块(JsRuntime::RunModuleSource): url 带递增查询串, 每次都是新模块;
//     相对导入按 baseDir 解析。
//   - momo 应用代码一律是 ESM(含 .js);node_modules 中的包保持 Node 的格式判定。
//   - 未捕获的异常和 Promise 拒绝只记录, 不退出进程(Node 默认退出;momo 应用出错后继续运行)。
const char kNodeBootstrapSource[] = R"JS(
'use strict';
const Module = require('module');
const fs = require('fs');
const path = require('path');
const { pathToFileURL, fileURLToPath } = require('url');
const { getOrInitializeCascadedLoader } = require('internal/modules/esm/loader');

process.on('uncaughtException', (error) => console.error('Uncaught exception:', error));
process.on('unhandledRejection', (reason) => console.error('Unhandled promise rejection:', reason));

if (codeCacheDir) Module.enableCompileCache(codeCacheDir);

const isNative = (specifier) => specifier.startsWith('momo:') || specifier.startsWith('ext:');
const hostSources = new Map();
let nextSourceId = 0;

function nativeModuleSource(url) {
  let binding;
  try {
    binding = process._linkedBinding(url);
  } catch {
    const error = new Error(`Cannot find native module '${url}'`);
    error.code = 'ERR_MODULE_NOT_FOUND';
    throw error;
  }
  let source = `const binding = process._linkedBinding(${JSON.stringify(url)});\n`;
  Object.keys(binding).forEach((name, i) => {
    source += `const v${i} = binding[${JSON.stringify(name)}];\n`;
    source += `export { v${i} as ${JSON.stringify(name)} };\n`;
  });
  return source;
}

Module.registerHooks({
  resolve(specifier, context, nextResolve) {
    if (isNative(specifier)) return { url: specifier, shortCircuit: true };
    if (hostSources.has(specifier)) return { url: specifier, format: 'module', shortCircuit: true };
    return nextResolve(specifier, context);
  },
  load(url, context, nextLoad) {
    if (isNative(url)) {
      return { format: 'module', source: nativeModuleSource(url), shortCircuit: true };
    }
    const hostSource = hostSources.get(url);
    if (hostSource !== undefined) {
      return { format: 'module', source: hostSource, shortCircuit: true };
    }
    const result = nextLoad(url, context);
    if (result.format === 'commonjs' && url.startsWith('file:') && !url.includes('/node_modules/') &&
        /\.m?js$/.test(new URL(url).pathname)) {
      return { format: 'module', source: fs.readFileSync(fileURLToPath(url)), shortCircuit: true };
    }
    return result;
  },
});

return {
  // 执行模块;返回其求值 Promise。source 为 undefined 时 url 是文件路径, 否则 url 是源码的名字。
  importModule(url, source, baseDir) {
    let target;
    if (source === undefined) {
      target = pathToFileURL(url).href;
    } else {
      const file = path.resolve(baseDir || process.cwd(), url);
      target = `${pathToFileURL(file).href}?momo-source=${++nextSourceId}`;
      hostSources.set(target, source);
    }
    const promise = getOrInitializeCascadedLoader().import(target, undefined, { __proto__: null },
                                                           undefined, true);
    promise.catch(() => {});  // 结果由宿主检查, 不算未处理的拒绝
    return promise;
  },
  flushCompileCache() {
    Module.flushCompileCache();
  },
};
)JS";

}  // namespace momo::engine::runtime
