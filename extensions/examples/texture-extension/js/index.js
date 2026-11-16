// @momo-examples/ext-texture: 外部纹理示例。一个带原生插件的普通 Node 包(三方扩展的样子, docs/07 §2);
// 用到宿主能力表, 所以把 momo:services 的 nativeHost 传给插件。类型在 index.d.ts。
import { nativeHost } from "momo:services";

import addon from "../build/ext_texture.node";

export function startPlasma(width, height) {
  return addon.start(nativeHost, width, height);
}

export function stopPlasma() {
  addon.stop();
}

export function framesProduced() {
  return addon.frames();
}
