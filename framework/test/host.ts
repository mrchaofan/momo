// 测试宿主抽象:测试写成"场景函数", 宿主可以是 Node + mock(本目录 mock/), 也可以是真实引擎
// (tools/momo-test 的 headless 运行器:加载 out/test/momo_tests.js 并调用 runTests(host))。

/** 记录的一次绘制/合成调用。 */
export interface RecordedOp {
  op: string;
  args: unknown[];
}

/** mock 宿主独有的观察/控制能力(真实引擎下为 null)。 */
export interface MockControl {
  paragraphsBuilt: number;
  paragraphLayouts: number;
  frameRequests: number;
  renderedScenes: number;
  lastSceneOps: RecordedOp[];
  cursor: string;
  title: string;
  assets: Map<string, Uint8Array>;
  decodedImages: number;
  setPhysicalSize(width: number, height: number, dpr: number): void;
  pictureOps(picture: unknown): RecordedOp[];
  // ---- 多视图 ----
  /** 每个视图最近一次提交的场景(键为视图 id)。 */
  sceneOpsByView: Map<number, RecordedOp[]>;
  /** 每个视图的标题。 */
  titles: Map<number, string>;
  /** 宿主是否已"打开"视图(createView 完成且未关闭)。 */
  openViews(): number[];
  /** 模拟用户点击某个视图的关闭按钮(询问 onCloseRequested, 允许则关闭)。返回是否关闭。 */
  requestCloseView(id: number): boolean;
  /** 下一次 createView 失败。 */
  failNextCreateView: boolean;
  /** 置顶状态(键为视图 id)。 */
  alwaysOnTop: Map<number, boolean>;
  /** activate() 调用过的视图 id(按顺序)。 */
  activated: number[];
  /** view.setCapture() 调用过的视图。 */
  captures: number[];
  /** view.dispatchPlatformViewPointer() 的调用(按顺序)。 */
  platformViewPointer: { view: number; platformViewId: number; nativeEventId: number; action: number }[];
  /** 以 panel: true 创建的视图。 */
  panelViews: Set<number>;
  /** 最近一次 createView 的参数。 */
  lastCreateViewOptions: { resizable?: boolean; panel?: boolean; width: number; height: number } | null;
  /** setTitleBarStyle 的最近一次设置(键为视图 id)。 */
  titleBars: Map<number, { style: string; center: number | null }>;
  /** setResizeOptions 的最近一次设置(键为视图 id)。 */
  resizeOptions: Map<number, { resizable?: boolean; border?: number | { left?: number; top?: number; right?: number; bottom?: number }; liveResizeFrameWait?: number }>;
  /** setDragRegions 的最近一次设置(键为视图 id, 扁平的 x,y,w,h)。 */
  dragRegions: Map<number, number[]>;
}

/** 测试宿主。 */
export interface TestHost {
  /** 宿主名("node-mock" / "engine-headless" ...)。 */
  readonly name: string;
  /** 输出一行日志。 */
  log(line: string): void;
  /** 文本是否为 Ahem 固定宽度模型(字形宽 = 字号, 行高 = 字号);为 false 时跳过依赖精确字形度量的断言。 */
  readonly ahemText: boolean;
  /** mock 观察能力;真实引擎为 null(依赖它的测试自动跳过)。 */
  readonly mock: MockControl | null;
  /** 只运行名称包含该子串的测试。 */
  readonly filter?: string;
}

/** runTests 的结果。 */
export interface TestSummary {
  passed: number;
  failed: number;
  skipped: number;
  failures: { name: string; error: string }[];
}
