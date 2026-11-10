// @momo-examples/ext-texture 的 JS 门面。返回的 textureId 交给框架的 Texture widget
// (或 SceneBuilder.addTexture / Canvas.drawTexture)显示。
import { native } from "ext:texture";

type Fn = (...args: unknown[]) => unknown;
const fns = native as unknown as Record<string, Fn>;

/** 开始生成动画帧, 返回纹理 id。 */
export function startPlasma(width: number, height: number): number {
  return fns.start(width, height) as number;
}

export function stopPlasma(): void {
  fns.stop();
}

export function framesProduced(): number {
  return fns.frames() as number;
}
