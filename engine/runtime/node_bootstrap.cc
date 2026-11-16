#include "engine/runtime/node_bootstrap.h"

namespace momo::engine::runtime {

// 在 Node 的 preload 阶段运行(早于任何用户代码), require 可访问 Node 内部模块——Node 版本由
// cmake/deps.lock.json 锁定, 升级时需核对这里用到的内部 API(internal/modules/esm/loader 的
// getOrInitializeCascadedLoader)。不用 runEntryPointWithESMLoader: 它把加载/求值错误当作未捕获异常
// 上报并让 Promise 正常结束, 宿主拿不到错误。
//
//   - momo:*: 运行时提供的模块, 由 module.registerHooks 解析。两种:
//       · 运行时自带的 JS 模块(框架、内置扩展): momo:<name> → <builtinDir>/<name>/package.json 的 main,
//         之后就是一个普通的文件模块(它的相对导入、它 import 的 .node 照常解析);
//       · 引擎的原生模块(momo:ui、momo:services): 模块内容来自 process._linkedBinding(宿主用
//         node::AddLinkedBinding 注册), 导出名在加载时从绑定对象读出。
//   - *.node(Node 原生插件, docs/07): `import addon from "./x.node"` 的默认导出是插件的 exports
//     (Node 自己只在 require 里认 .node)。
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

const isNative = (specifier) => specifier.startsWith('momo:');
// momo:<name> → 运行时自带的 JS 模块的入口(file: URL);没有这个模块为 null(那就是引擎的原生模块)。
const builtinEntries = new Map();
function builtinEntry(specifier) {
  if (!builtinEntries.has(specifier)) {
    let entry = null;
    const name = specifier.slice('momo:'.length);
    if (builtinDir && /^[a-z][a-z0-9_-]*$/.test(name)) {
      const dir = path.join(builtinDir, name);
      try {
        const main = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')).main;
        entry = pathToFileURL(path.join(dir, typeof main === 'string' ? main : 'index.js')).href;
      } catch {
        // 没有这个目录(或它不是一个包): 不是自带的 JS 模块。
      }
    }
    builtinEntries.set(specifier, entry);
  }
  return builtinEntries.get(specifier);
}
const isAddon = (url) => url.startsWith('file:') && new URL(url).pathname.endsWith('.node');
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
    if (isNative(specifier)) return { url: builtinEntry(specifier) ?? specifier, shortCircuit: true };
    if (hostSources.has(specifier)) return { url: specifier, format: 'module', shortCircuit: true };
    return nextResolve(specifier, context);
  },
  load(url, context, nextLoad) {
    if (isNative(url)) {
      return { format: 'module', source: nativeModuleSource(url), shortCircuit: true };
    }
    if (isAddon(url)) {
      const source = `import { createRequire } from 'node:module';\n` +
          `export default createRequire(import.meta.url)(${JSON.stringify(fileURLToPath(url))});\n`;
      return { format: 'module', source, shortCircuit: true };
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
  // 执行模块;返回 run = { promise, started, detached }。source 为 undefined 时 url 是文件路径, 否则 url 是
  // 源码的名字。promise 是求值结果;started 在模块图加载、链接完成, 入口即将求值时置位(入口被包在一个
  // 先导入标记模块的包装模块里, 标记模块求值时回调这里);宿主不再等待结果时置 detached, 此后的失败在这里记录。
  importModule(url, source, baseDir) {
    let target;
    if (source === undefined) {
      target = pathToFileURL(url).href;
    } else {
      const file = path.resolve(baseDir || process.cwd(), url);
      target = `${pathToFileURL(file).href}?momo-source=${++nextSourceId}`;
      hostSources.set(target, source);
    }
    const run = { promise: null, started: false, detached: false };
    const id = ++nextSourceId;
    const marker = `momo-entry:marker-${id}`;
    const wrapper = `momo-entry:run-${id}`;
    // 标记模块只能经全局对象回调: 用一个一次性的属性, 用完即删。
    const hook = Symbol.for(marker);
    globalThis[hook] = () => { delete globalThis[hook]; run.started = true; };
    hostSources.set(marker, `globalThis[Symbol.for(${JSON.stringify(marker)})]();`);
    hostSources.set(wrapper, `import ${JSON.stringify(marker)};\nimport ${JSON.stringify(target)};`);
    run.promise = getOrInitializeCascadedLoader().import(wrapper, undefined, { __proto__: null },
                                                         undefined, true);
    run.promise.catch((error) => {
      // 宿主还在等待时由它检查结果;已不再等待(入口停在顶层 await)时在这里记录。
      if (run.detached) console.error('Uncaught error in the entry module:', error);
    }).finally(() => {
      delete globalThis[hook];
      hostSources.delete(marker);
      hostSources.delete(wrapper);
    });
    return run;
  },
  flushCompileCache() {
    Module.flushCompileCache();
  },
};
)JS";

}  // namespace momo::engine::runtime
