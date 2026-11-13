import { reportError } from "../foundation/assert";
import type { Key } from "../foundation/key";
import { RenderPlatformView } from "../rendering/platform_view";
import { PlatformViewsService, type PlatformViewController } from "../services/platform_views";
import { SizedBox } from "./basic";
import { FocusNode } from "./focus_manager";
import { Focus } from "./focus_scope";
import { LeafRenderObjectView, State, StatefulView, type BuildContext, type View } from "./framework";

/** 已创建视图的占位渲染(内部)。 */
class PlatformViewSurface extends LeafRenderObjectView {
  constructor(readonly viewId: number) {
    super(null);
  }
  createRenderObject(): RenderPlatformView {
    return new RenderPlatformView(this.viewId);
  }
  override updateRenderObject(_c: BuildContext, r: RenderPlatformView): void {
    r.viewId = this.viewId;
  }
}

/** PlatformView 参数。 */
export interface PlatformViewOptions {
  key?: Key | null;
  /** 扩展注册的视图类型, 如 "momo.webview"。变化时销毁旧视图并创建新视图。 */
  viewType: string;
  /** 传给扩展工厂的创建参数(只在创建时使用)。 */
  creationParams?: Uint8Array | null;
  /** 视图创建成功。 */
  onCreated?: ((controller: PlatformViewController) => void) | null;
  /** 创建失败(如类型未注册)。缺省时报告到错误处理。 */
  onCreateError?: ((error: unknown) => void) | null;
  /** 视图创建完成前显示的内容(缺省为空白, 尺寸取约束最大值)。 */
  placeholder?: View | null;
  /** 参与 momo 焦点系统的节点(缺省内部创建)。获得焦点时把键盘焦点交给原生视图。 */
  focusNode?: FocusNode | null;
  autofocus?: boolean;
}

/**
 * 嵌入由扩展提供的系统原生视图(macOS: NSView, 如 WKWebView), docs/05 §9。
 * 视图与 momo 画面按绘制顺序叠放: 之后绘制的 momo 内容(对话框、浮层)盖在视图上, 且那片区域的点击归 momo。
 * 尺寸取约束的最大值(放进有界约束中, 如 SizedBox / Expanded)。
 * 焦点: 用户点击原生视图时, 本组件的 FocusNode 成为主焦点(momo 的输入框随之失焦);
 *       在 momo 里请求本组件焦点时, 键盘焦点交给原生视图。
 * 限制: 只支持平移与缩放(旋转/斜切按包围盒近似);圆角裁剪只支持与可见区域贴合的一层。
 */
export class PlatformView extends StatefulView {
  readonly opts: PlatformViewOptions;
  constructor(o: PlatformViewOptions) {
    super(o.key);
    this.opts = o;
  }
  createState(): State {
    return new PlatformViewState();
  }
}

export class PlatformViewState extends State<PlatformView> {
  #controller: PlatformViewController | null = null;
  #generation = 0;  // 每次(重新)创建递增: 丢弃过期的异步结果
  #internalNode: FocusNode | null = null;
  #focusFromNative = false;

  /** 已创建的视图(创建中/失败时为 null)。 */
  get controller(): PlatformViewController | null {
    return this.#controller;
  }

  get #focusNode(): FocusNode {
    return this.view.opts.focusNode ?? (this.#internalNode ??= new FocusNode({ debugLabel: "PlatformView" }));
  }

  override initState(): void {
    super.initState();
    this.#create();
  }

  override didUpdateView(old: PlatformView): void {
    super.didUpdateView(old);
    if (old.opts.viewType !== this.view.opts.viewType) {
      this.#disposeController();
      this.#create();
    }
  }

  override dispose(): void {
    this.#generation++;
    this.#disposeController();
    this.#internalNode?.dispose();
    this.#internalNode = null;
    super.dispose();
  }

  #create(): void {
    const generation = ++this.#generation;
    const { viewType, creationParams } = this.view.opts;
    PlatformViewsService.create(viewType, creationParams ?? null).then(
      (controller) => {
        if (generation !== this.#generation || !this.mounted) {
          // 创建期间组件已销毁或类型已变: 立即释放。
          controller.dispose().catch((e: unknown) => reportError(e, "disposing a stale platform view"));
          return;
        }
        controller.setOnFocused(() => this.#onNativeFocused());
        this.setState(() => {
          this.#controller = controller;
        });
        this.view.opts.onCreated?.(controller);
      },
      (e: unknown) => {
        if (generation !== this.#generation || !this.mounted) return;
        const onError = this.view.opts.onCreateError;
        if (onError) onError(e);
        else reportError(e, `creating platform view '${viewType}'`);
      },
    );
  }

  #disposeController(): void {
    const controller = this.#controller;
    this.#controller = null;
    if (controller !== null) {
      controller.dispose().catch((e: unknown) => reportError(e, "disposing a platform view"));
    }
  }

  // 原生视图因用户点击获得了键盘焦点: 让 momo 的焦点跟过来(不要再反向 focus 原生视图)。
  #onNativeFocused(): void {
    const node = this.#focusNode;
    if (node.hasPrimaryFocus) return;
    this.#focusFromNative = true;
    node.requestFocus();
  }

  #onFocusChange = (focused: boolean): void => {
    if (!focused) return;
    if (this.#focusFromNative) {
      this.#focusFromNative = false;
      return;
    }
    this.#controller?.focus().catch((e: unknown) => reportError(e, "focusing a platform view"));
  };

  build(_context: BuildContext): View {
    const controller = this.#controller;
    const child = controller !== null
      ? new PlatformViewSurface(controller.viewId)
      : (this.view.opts.placeholder ?? SizedBox.expand());
    return new Focus({
      focusNode: this.#focusNode,
      autofocus: this.view.opts.autofocus ?? false,
      onFocusChange: this.#onFocusChange,
      child,
    });
  }
}
