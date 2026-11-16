// @momo-examples/ext-texture 的 JS 门面。返回的 textureId 交给框架的 Texture view
// (或 SceneBuilder.addTexture / Canvas.drawTexture)显示。
import { nativeHost } from "momo:services";
import addon from "../build/ext_texture.node";

const native = addon as {
  start(host: bigint, width: number, height: number): number;
  stop(): void;
  frames(): number;
};

/** 开始生成动画帧, 返回纹理 id。 */
export function startPlasma(width: number, height: number): number {
  return native.start(nativeHost, width, height);
}

export function stopPlasma(): void {
  native.stop();
}

export function framesProduced(): number {
  return native.frames();
}
