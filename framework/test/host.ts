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
  // ---- 多视图与宿主窗口(momo/window 的 mock 实现)----
  /** 每个视图最近一次提交的场景(键为视图 id)。 */
  sceneOpsByView: Map<number, RecordedOp[]>;
  /** 每个视图的标题。 */
  titles: Map<number, string>;
  /** 宿主是否已"打开"视图(创建完成且未关闭;不含视图 0)。 */
  openViews(): number[];
  /** 视图是否存在(含视图 0)。 */
  hasView(id: number): boolean;
  /** 模拟用户点击某个视图的关闭按钮(询问 JS 的 closeRequested, 允许则关闭)。返回是否关闭。 */
  requestCloseView(id: number): Promise<boolean>;
  /** 模拟用户/系统请求退出应用(momo/app requestExit)。返回是否允许。 */
  requestAppExit(): Promise<boolean>;
  /** 下一次 create 失败。 */
  failNextCreateView: boolean;
  /** 置顶状态(键为视图 id)。 */
  alwaysOnTop: Map<number, boolean>;
  /** activate 调用过的视图 id(按顺序)。 */
  activated: number[];
  /** momo/window capturePointer 调用过的视图(原生接收窗口改交)。 */
  captures: number[];
  /** 以 nonActivating: true 创建的视图。 */
  panelViews: Set<number>;
  /** 最近一次 create 的参数。 */
  lastCreateViewOptions: { resizable?: boolean; nonActivating?: boolean; width: number; height: number } | null;
  /** setTitleBar 的最近一次设置(键为视图 id)。 */
  titleBars: Map<number, { hidden: boolean; height: number }>;
  /** setResizable 的最近一次设置(键为视图 id)。 */
  resizable: Map<number, boolean>;
  /** setResizeBorder 合并后的边框(键为视图 id)。 */
  resizeBorders: Map<number, { left?: number; top?: number; right?: number; bottom?: number }>;
  /** 宿主侧的窗口屏幕框(键为视图 id)。 */
  frames: Map<number, { x: number; y: number; width: number; height: number }>;
  /** startDrag 的调用(按顺序)。 */
  windowDrags: { view: number; sequenceId: number }[];
  /** 进行中的移动循环(键为视图 id, 值为抓取偏移)。 */
  moveLoops: Map<number, { x: number; y: number }>;
  /** setMinSize 的最近一次设置。 */
  minSizes: Map<number, { width: number; height: number }>;
  /** 以宿主身份投递指针包(先执行移动循环, 再交给视图)。 */
  deliverPointer(view: number, data: Float64Array, count: number): void;
  /** 测试复位: 视图 0 被关闭过时重新打开它。 */
  debugRestoreImplicitView(): void;
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
