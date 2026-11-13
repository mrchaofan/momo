import type { Image as UiImage } from "momo:ui";
import type { Key } from "../foundation/key";
import { Alignment } from "../painting/alignment";
import type { BoxFit } from "../painting/box_fit";
import { AssetImage, MemoryImage, type ImageInfo, type ImageProvider, type ImageStreamCompleter, type ImageStreamListener } from "../painting/image_provider";
import { RenderImage } from "../rendering/image";
import { LeafRenderObjectView, State, StatefulView, type BuildContext, type View } from "./framework";

/** 直接显示已解码图片。 */
export class RawImage extends LeafRenderObjectView {
  readonly image: UiImage | null;
  readonly width: number | null;
  readonly height: number | null;
  readonly scale: number;
  readonly fit: BoxFit | null;
  readonly alignment: Alignment;
  readonly opacity: number;
  constructor(o: { key?: Key | null; image: UiImage | null; width?: number | null; height?: number | null; scale?: number; fit?: BoxFit | null; alignment?: Alignment; opacity?: number }) {
    super(o.key);
    this.image = o.image;
    this.width = o.width ?? null;
    this.height = o.height ?? null;
    this.scale = o.scale ?? 1;
    this.fit = o.fit ?? null;
    this.alignment = o.alignment ?? Alignment.center;
    this.opacity = o.opacity ?? 1;
  }
  createRenderObject(): RenderImage {
    return new RenderImage({ image: this.image, width: this.width, height: this.height, scale: this.scale, fit: this.fit, alignment: this.alignment, opacity: this.opacity });
  }
  override updateRenderObject(_c: BuildContext, r: RenderImage): void {
    r.image = this.image;
    r.width = this.width;
    r.height = this.height;
    r.scale = this.scale;
    r.fit = this.fit;
    r.alignment = this.alignment;
    r.opacity = this.opacity;
  }
}

/** Image 参数。 */
export interface ImageOptions {
  key?: Key | null;
  width?: number | null;
  height?: number | null;
  fit?: BoxFit | null;
  alignment?: Alignment;
  opacity?: number;
  /** 加载中/失败时显示。 */
  placeholder?: View | null;
  errorBuilder?: ((context: BuildContext, error: unknown) => View) | null;
}

/** 从 ImageProvider 异步加载并显示图片(经 ImageCache)。 */
export class Image extends StatefulView {
  readonly image: ImageProvider;
  readonly opts: ImageOptions;
  constructor(image: ImageProvider, o: ImageOptions = {}) {
    super(o.key);
    this.image = image;
    this.opts = o;
  }
  /** 应用资源图片。 */
  static asset(name: string, o: ImageOptions & { scale?: number } = {}): Image {
    return new Image(new AssetImage(name, { scale: o.scale ?? 1 }), o);
  }
  /** 内存字节图片。 */
  static memory(bytes: Uint8Array | ArrayBuffer, o: ImageOptions & { scale?: number } = {}): Image {
    return new Image(new MemoryImage(bytes, { scale: o.scale ?? 1 }), o);
  }
  createState(): State {
    return new ImageState();
  }
}

export class ImageState extends State<Image> {
  #completer: ImageStreamCompleter | null = null;
  #info: ImageInfo | null = null;
  #error: unknown = null;
  #hasError = false;
  #listener: ImageStreamListener = {
    onImage: (info: ImageInfo, sync: boolean) => {
      if (sync) this.#info = info;
      else this.setState(() => (this.#info = info));
    },
    onError: (e: unknown) => {
      if (this.mounted) this.setState(() => {
        this.#error = e;
        this.#hasError = true;
      });
    },
  };

  override didChangeDependencies(): void {
    if (this.#completer === null) this.#resolve();
  }
  override didUpdateView(old: Image): void {
    if (!old.image.equals(this.view.image)) this.#resolve();
  }
  override dispose(): void {
    this.#completer?.removeListener(this.#listener);
  }
  #resolve(): void {
    const next = this.view.image.resolve();
    if (next === this.#completer) return;
    this.#completer?.removeListener(this.#listener);
    this.#info = null;
    this.#hasError = false;
    this.#completer = next;
    next.addListener(this.#listener);
  }

  build(context: BuildContext): View {
    const o = this.view.opts;
    if (this.#hasError && o.errorBuilder) return o.errorBuilder(context, this.#error);
    if (this.#info === null && o.placeholder) return o.placeholder;
    return new RawImage({
      image: this.#info?.image ?? null,
      scale: this.#info?.scale ?? 1,
      width: o.width ?? null,
      height: o.height ?? null,
      fit: o.fit ?? null,
      alignment: o.alignment ?? Alignment.center,
      opacity: o.opacity ?? 1,
    });
  }
}
