/**
 * 开始生成动画帧, 返回纹理 id: 交给框架的 Texture view(或 SceneBuilder.addTexture)显示。
 */
export function startPlasma(width: number, height: number): number;
export function stopPlasma(): void;
export function framesProduced(): number;
