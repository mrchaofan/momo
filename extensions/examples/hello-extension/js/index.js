// @momo-examples/ext-hello: 一个带原生插件的普通 Node 包(三方扩展的样子, docs/07 §2)。它要保持原样、留在应用的
// node_modules 里由 Node 加载——这里按相对路径找自己的 .node;应用自己的构建不要把它编译进入口文件。
// 类型在 index.d.ts。
import addon from "../build/ext_hello.node";

export const { add, greet, reverseBytes, hash } = addon;
