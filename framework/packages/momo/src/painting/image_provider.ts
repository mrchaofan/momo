import { decodeImage, loadAsset, type Image } from "momo:ui";
import { reportError } from "../foundation/assert";

/** 已解码图片 + 缩放(物理像素 / 逻辑像素)。 */
export interface ImageInfo {
  readonly image: Image;
  readonly scale: number;
}

/** 图片流监听者:成功给 info, 失败给 error。 */
export interface ImageStreamListener {
  onImage(info: ImageInfo, synchronousCall: boolean): void;
  onError?(error: unknown): void;
}

/** 单张图片的加载结果分发器(可被多个使用者共享)。 */
export class ImageStreamCompleter {
  #info: ImageInfo | null = null;
  #error: unknown = null;
  #hasError = false;
  #listeners: ImageStreamListener[] = [];

  get currentImage(): ImageInfo | null {
    return this.#info;
  }
  get hasListeners(): boolean {
    return this.#listeners.length > 0;
  }

  addListener(listener: ImageStreamListener): void {
    this.#listeners.push(listener);
    if (this.#info !== null) listener.onImage(this.#info, true);
    else if (this.#hasError) listener.onError?.(this.#error);
  }

  removeListener(listener: ImageStreamListener): void {
    const i = this.#listeners.indexOf(listener);
    if (i >= 0) this.#listeners.splice(i, 1);
  }

  /** @internal */
  setImage(info: ImageInfo): void {
    this.#info = info;
    for (const l of this.#listeners.slice()) {
      try {
        l.onImage(info, false);
      } catch (e) {
        reportError(e, "in an image listener");
      }
    }
  }

  /** @internal */
  setError(error: unknown): void {
    this.#error = error;
    this.#hasError = true;
    let handled = false;
    for (const l of this.#listeners.slice()) {
      if (l.onError) {
        handled = true;
        l.onError(error);
      }
    }
    if (!handled) reportError(error, "resolving an image");
  }
}

interface CacheEntry {
  completer: ImageStreamCompleter;
  sizeBytes: number;
}

/** 图片缓存:LRU, 按条目数与字节数(宽×高×4)双重上限。 */
export class ImageCache {
  #entries = new Map<string, CacheEntry>();
  #bytes = 0;
  #maximumSize = 1000;
  #maximumSizeBytes = 100 << 20;

  get maximumSize(): number {
    return this.#maximumSize;
  }
  set maximumSize(v: number) {
    this.#maximumSize = v;
    this.#checkSize();
  }
  get maximumSizeBytes(): number {
    return this.#maximumSizeBytes;
  }
  set maximumSizeBytes(v: number) {
    this.#maximumSizeBytes = v;
    this.#checkSize();
  }
  get currentSize(): number {
    return this.#entries.size;
  }
  get currentSizeBytes(): number {
    return this.#bytes;
  }
  containsKey(key: string): boolean {
    return this.#entries.has(key);
  }

  /** 取缓存(命中则移到最新), 否则用 loader 创建并缓存。 */
  putIfAbsent(key: string, loader: () => ImageStreamCompleter): ImageStreamCompleter {
    const hit = this.#entries.get(key);
    if (hit !== undefined) {
      this.#entries.delete(key);
      this.#entries.set(key, hit);
      return hit.completer;
    }
    const completer = loader();
    const entry: CacheEntry = { completer, sizeBytes: 0 };
    this.#entries.set(key, entry);
    completer.addListener({
      onImage: (info) => {
        if (this.#entries.get(key) !== entry) return;
        this.#bytes -= entry.sizeBytes;
        entry.sizeBytes = info.image.width * info.image.height * 4;
        this.#bytes += entry.sizeBytes;
        this.#checkSize();
      },
      onError: () => {
        // 失败的条目不缓存, 以便重试。
        if (this.#entries.get(key) === entry) this.evict(key);
      },
    });
    this.#checkSize();
    return completer;
  }

  /** 移除一项。 */
  evict(key: string): boolean {
    const e = this.#entries.get(key);
    if (e === undefined) return false;
    this.#bytes -= e.sizeBytes;
    this.#entries.delete(key);
    return true;
  }

  /** 清空。 */
  clear(): void {
    this.#entries.clear();
    this.#bytes = 0;
  }

  #checkSize(): void {
    while (this.#entries.size > this.#maximumSize || this.#bytes > this.#maximumSizeBytes) {
      const oldest = this.#entries.keys().next().value;
      if (oldest === undefined) break;
      this.evict(oldest);
    }
  }
}

/** 全局图片缓存。 */
export const imageCache = new ImageCache();

/** 图片来源抽象:提供缓存键与加载逻辑。 */
export abstract class ImageProvider {
  /** 物理像素/逻辑像素比。 */
  readonly scale: number;
  constructor(scale = 1) {
    this.scale = scale;
  }
  /** 缓存键(值语义)。 */
  abstract get cacheKey(): string;
  /** 加载并解码。 */
  protected abstract load(): Promise<Image>;

  /** 解析为(缓存的)图片流。 */
  resolve(): ImageStreamCompleter {
    return imageCache.putIfAbsent(this.cacheKey, () => {
      const c = new ImageStreamCompleter();
      this.load().then(
        (image) => c.setImage({ image, scale: this.scale }),
        (e: unknown) => c.setError(e),
      );
      return c;
    });
  }

  equals(other: ImageProvider | null | undefined): boolean {
    return other != null && other.cacheKey === this.cacheKey;
  }
}

/** 应用资源图片(assets/ 下, 经 momo:ui loadAsset + decodeImage)。 */
export class AssetImage extends ImageProvider {
  constructor(readonly assetName: string, o: { scale?: number } = {}) {
    super(o.scale ?? 1);
  }
  get cacheKey(): string {
    return `asset:${this.assetName}@${this.scale}`;
  }
  protected async load(): Promise<Image> {
    const bytes = await loadAsset(this.assetName);
    return decodeImage(bytes);
  }
}

const memoryIds = new WeakMap<object, number>();
let nextMemoryId = 1;

/** 内存字节图片(以字节缓冲对象身份作缓存键)。 */
export class MemoryImage extends ImageProvider {
  readonly #id: number;
  constructor(readonly bytes: Uint8Array | ArrayBuffer, o: { scale?: number } = {}) {
    super(o.scale ?? 1);
    let id = memoryIds.get(bytes);
    if (id === undefined) {
      id = nextMemoryId++;
      memoryIds.set(bytes, id);
    }
    this.#id = id;
  }
  get cacheKey(): string {
    return `memory:${this.#id}@${this.scale}`;
  }
  protected load(): Promise<Image> {
    return decodeImage(this.bytes);
  }
}
