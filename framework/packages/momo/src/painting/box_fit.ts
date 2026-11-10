import type { Canvas, FilterQuality, Image } from "momo:ui";
import { Paint } from "momo:ui";
import { Alignment } from "./alignment";
import { Rect, Size } from "./geometry";

/** 图片在盒子里的适配方式。 */
export type BoxFit = "fill" | "contain" | "cover" | "fitWidth" | "fitHeight" | "none" | "scaleDown";

/** 适配结果:源尺寸(图片坐标)与目标尺寸。 */
export interface FittedSizes {
  source: Size;
  destination: Size;
}

/** 计算 BoxFit 的源/目标尺寸(与 Flutter applyBoxFit 一致)。 */
export function applyBoxFit(fit: BoxFit, input: Size, output: Size): FittedSizes {
  if (input.height <= 0 || input.width <= 0 || output.height <= 0 || output.width <= 0) {
    return { source: Size.zero, destination: Size.zero };
  }
  let source: Size;
  let destination: Size;
  switch (fit) {
    case "fill":
      source = input;
      destination = output;
      break;
    case "contain":
      source = input;
      destination = output.width / output.height > input.width / input.height
        ? new Size((input.width * output.height) / input.height, output.height)
        : new Size(output.width, (input.height * output.width) / input.width);
      break;
    case "cover":
      source = output.width / output.height > input.width / input.height
        ? new Size(input.width, (input.width * output.height) / output.width)
        : new Size((input.height * output.width) / output.height, input.height);
      destination = output;
      break;
    case "fitWidth":
      source = new Size(input.width, Math.min(input.height, (input.width * output.height) / output.width));
      destination = new Size(output.width, (source.height * output.width) / source.width);
      break;
    case "fitHeight":
      source = new Size(Math.min(input.width, (input.height * output.width) / output.height), input.height);
      destination = new Size((source.width * output.height) / source.height, output.height);
      break;
    case "none":
      source = new Size(Math.min(input.width, output.width), Math.min(input.height, output.height));
      destination = source;
      break;
    case "scaleDown": {
      source = input;
      destination = input;
      const aspect = input.width / input.height;
      if (destination.height > output.height) destination = new Size(output.height * aspect, output.height);
      if (destination.width > output.width) destination = new Size(output.width, output.width / aspect);
      break;
    }
  }
  return { source, destination };
}

const imagePaint = new Paint();

/** 把图片按 fit/alignment 画进 rect。 */
export function paintImage(o: {
  canvas: Canvas;
  rect: Rect;
  image: Image;
  fit?: BoxFit;
  alignment?: Alignment;
  opacity?: number;
  filterQuality?: FilterQuality;
}): void {
  const { canvas, rect, image } = o;
  if (rect.isEmpty) return;
  const inputSize = new Size(image.width, image.height);
  const fit = o.fit ?? "scaleDown";
  const alignment = o.alignment ?? Alignment.center;
  const fitted = applyBoxFit(fit, inputSize, rect.size);
  const dst = alignment.inscribe(fitted.destination, rect);
  const src = alignment.inscribe(fitted.source, Rect.fromLTWH(0, 0, inputSize.width, inputSize.height));
  imagePaint.color = Math.round(Math.min(1, Math.max(0, o.opacity ?? 1)) * 255) * 0x1000000;
  imagePaint.filterQuality = o.filterQuality ?? "low";
  const needClip = dst.left < rect.left || dst.top < rect.top || dst.right > rect.right || dst.bottom > rect.bottom;
  if (needClip) {
    canvas.save();
    canvas.clipRect(rect.left, rect.top, rect.width, rect.height);
  }
  canvas.drawImageRect(image, src.left, src.top, src.width, src.height, dst.left, dst.top, dst.width, dst.height, imagePaint);
  if (needClip) canvas.restore();
}
