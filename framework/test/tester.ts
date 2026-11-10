// WidgetTester:对标 flutter_test,通过 window 回调驱动帧、指针、键盘,经 BinaryMessenger 模拟平台消息。
// 只依赖框架公开的 window 回调与 messenger,因此在 mock 宿主和真实引擎宿主下都可用。
import { views, window } from "momo:ui";
import {
  Element, GlobalKey, Key, Offset, Rect, RenderBox, RichText, SchedulerBinding, Size, StandardMethodCodec, MethodCall,
  WidgetsBinding, defaultBinaryMessenger, keysEqual, runApp, paragraphCache, type Type, type Widget, HardwareKeyboard,
  FocusManager, imageCache, TextInput,
} from "../packages/momo/src/index";

/** 指针设备类型码(与 momo:ui 指针包一致)。 */
export const Kind = { mouse: 0, trackpad: 1, touch: 2, stylus: 3 } as const;
/** phase 码。 */
export const Phase = { add: 0, down: 1, move: 2, up: 3, remove: 4, hover: 5, scroll: 6 } as const;

export interface PointerOptions {
  kind?: number;
  device?: number;
  buttons?: number;
  scrollDeltaX?: number;
  scrollDeltaY?: number;
  /** 目标视图(窗口)id, 缺省主窗口。坐标为该窗口的逻辑坐标。 */
  view?: number;
}

/** 测试驱动器。 */
export class WidgetTester {
  readonly binding: WidgetsBinding;
  /** 假时钟(毫秒)。 */
  now = 1000;
  #dpr = 1;

  constructor(size: Size = new Size(800, 600), devicePixelRatio = 1) {
    this.binding = WidgetsBinding.ensureInitialized();
    this.#dpr = devicePixelRatio;
    this.binding.debugOverrideView(size, devicePixelRatio);
  }

  get devicePixelRatio(): number {
    return this.#dpr;
  }

  /** 设置视图逻辑尺寸。 */
  setViewSize(size: Size, dpr = this.#dpr): void {
    this.#dpr = dpr;
    this.binding.debugOverrideView(size, dpr);
  }

  /** 清空微任务队列(若干轮)。 */
  async flushMicrotasks(): Promise<void> {
    for (let i = 0; i < 10; i++) await Promise.resolve();
  }

  /** runApp 并出一帧。 */
  async pumpWidget(widget: Widget, durationMs = 0): Promise<void> {
    runApp(widget);
    await this.pump(durationMs);
  }

  /** 时钟前进 durationMs 并出一帧。 */
  async pump(durationMs = 0): Promise<void> {
    this.now += durationMs;
    await this.flushMicrotasks();
    window.onBeginFrame?.(this.now);
    await this.flushMicrotasks();
    window.onDrawFrame?.();
    await this.flushMicrotasks();
  }

  /** 以 stepMs 为间隔出帧, 共 totalMs。 */
  async pumpFor(totalMs: number, stepMs = 16): Promise<void> {
    for (let t = 0; t < totalMs; t += stepMs) await this.pump(Math.min(stepMs, totalMs - t));
  }

  /** 持续出帧直到不再请求新帧(动画结束)。 */
  async pumpAndSettle(stepMs = 16, maxMs = 10000): Promise<number> {
    let elapsed = 0;
    await this.pump();
    while (SchedulerBinding.instance.hasScheduledFrame) {
      if (elapsed > maxMs) throw new Error("pumpAndSettle timed out");
      await this.pump(stepMs);
      elapsed += stepMs;
    }
    return elapsed;
  }

  // ---------------------------------------------------------------------------
  // 查找

  /** 全部 Element(深度优先)。 */
  allElements(): Element[] {
    const out: Element[] = [];
    const root = this.binding.rootElement;
    const walk = (e: Element): void => {
      out.push(e);
      e.visitChildren(walk);
    };
    if (root) walk(root);
    return out;
  }
  byType(type: Type<Widget>): Element[] {
    return this.allElements().filter((e) => e.widget instanceof type);
  }
  byKey(key: Key): Element[] {
    return this.allElements().filter((e) => keysEqual(e.widget.key, key));
  }
  /** 显示该文本(完全相等)的 RichText Element。 */
  byText(text: string): Element[] {
    return this.allElements().filter((e) => e.widget instanceof RichText && e.widget.text.toPlainText() === text);
  }
  /** 唯一匹配, 否则抛错。 */
  one(list: Element[], what = "element"): Element {
    if (list.length !== 1) throw new Error(`expected exactly one ${what}, found ${list.length}`);
    return list[0]!;
  }
  widget<T extends Widget>(type: Type<T>): T {
    return this.one(this.byType(type), type.name).widget as T;
  }
  state<T>(key: GlobalKey): T {
    return key.currentState as T;
  }

  /** 元素的全局矩形(逻辑像素)。 */
  rectOf(e: Element): Rect {
    const ro = e.findRenderObject();
    if (!(ro instanceof RenderBox)) throw new Error(`${e.toStringShort()} has no RenderBox`);
    const tl = ro.localToGlobal(Offset.zero);
    return Rect.fromLTWH(tl.dx, tl.dy, ro.size.width, ro.size.height);
  }
  centerOf(e: Element): Offset {
    return this.rectOf(e).center;
  }

  // ---------------------------------------------------------------------------
  // 指针

  /** 发送一个指针事件(逻辑坐标)。 */
  sendPointer(phase: number, position: Offset, o: PointerOptions = {}): void {
    const d = new Float64Array(10);
    d[0] = this.now * 1000;
    d[1] = o.kind ?? Kind.mouse;
    d[2] = phase;
    d[3] = o.device ?? (o.kind === Kind.touch ? 1 : 0);
    d[4] = position.dx * this.#dpr;
    d[5] = position.dy * this.#dpr;
    d[6] = o.buttons ?? (phase === Phase.down || phase === Phase.move ? 1 : 0);
    d[7] = (o.scrollDeltaX ?? 0) * this.#dpr;
    d[8] = (o.scrollDeltaY ?? 0) * this.#dpr;
    d[9] = 0;
    const target = o.view !== undefined && o.view !== 0 ? views.get(o.view) : window;
    target?.onPointerPacket?.(d, 1);
  }

  #pos(target: Element | Offset): Offset {
    return target instanceof Offset ? target : this.centerOf(target);
  }

  /** 单击(按下 → 抬起), 之后出一帧。 */
  async tap(target: Element | Offset, o: PointerOptions = {}): Promise<void> {
    const p = this.#pos(target);
    this.sendPointer(Phase.down, p, o);
    await this.flushMicrotasks();
    this.now += 50;
    this.sendPointer(Phase.up, p, { ...o, buttons: 0 });
    await this.pump();
  }

  /** 拖动:按下, 分 steps 次移动 delta, 抬起。 */
  async drag(target: Element | Offset, delta: Offset, o: PointerOptions & { steps?: number; stepMs?: number } = {}): Promise<void> {
    const start = this.#pos(target);
    const steps = o.steps ?? 10;
    this.sendPointer(Phase.down, start, o);
    await this.flushMicrotasks();
    for (let i = 1; i <= steps; i++) {
      this.now += o.stepMs ?? 16;
      this.sendPointer(Phase.move, start.plus(delta.scale(i / steps)), o);
      await this.flushMicrotasks();
    }
    this.now += o.stepMs ?? 16;
    this.sendPointer(Phase.up, start.plus(delta), { ...o, buttons: 0 });
    await this.pump();
  }

  /** 鼠标滚轮(逻辑像素, 正值向下)。 */
  async scroll(at: Element | Offset, dy: number, dx = 0): Promise<void> {
    this.sendPointer(Phase.scroll, this.#pos(at), { kind: Kind.mouse, scrollDeltaX: dx, scrollDeltaY: dy, buttons: 0 });
    await this.pump();
  }

  /** 鼠标悬停移动。 */
  async hover(at: Element | Offset): Promise<void> {
    this.sendPointer(Phase.hover, this.#pos(at), { kind: Kind.mouse, buttons: 0 });
    await this.pump();
  }

  // ---------------------------------------------------------------------------
  // 键盘

  /** 发送一条 KeyData;返回框架是否消费。 */
  sendKeyData(type: "down" | "up" | "repeat", logicalKey: number, physicalKey: number, o: { character?: string; modifiers?: number } = {}): boolean {
    const r = window.onKeyData?.({ type, timeStampUs: this.now * 1000, physicalKey, logicalKey, character: o.character ?? "", modifiers: o.modifiers ?? 0 });
    return r === true;
  }

  /** 按下并抬起一个键, 之后出一帧。返回按下是否被消费。 */
  async pressKey(key: { logical: number; physical: number; character?: string }, modifiers = 0): Promise<boolean> {
    const handled = this.sendKeyData("down", key.logical, key.physical, { character: key.character ?? "", modifiers });
    this.sendKeyData("up", key.logical, key.physical, { modifiers });
    await this.pump();
    return handled;
  }

  // ---------------------------------------------------------------------------
  // 平台通道

  /** 模拟原生 → JS 方法调用(StandardMethodCodec)。 */
  async platformCall(channel: string, method: string, args: unknown = null): Promise<void> {
    const data = StandardMethodCodec.instance.encodeMethodCall(new MethodCall(method, args));
    await defaultBinaryMessenger.handlePlatformMessage(channel, data);
    await this.flushMicrotasks();
  }

  /** 记录 JS → 原生 的方法调用(返回调用列表), 回复 null 成功信封。 */
  recordMethodCalls(channel: string, reply: (call: MethodCall) => unknown = () => null): MethodCall[] {
    const calls: MethodCall[] = [];
    defaultBinaryMessenger.setMockMessageHandler(channel, (data) => {
      const call = StandardMethodCodec.instance.decodeMethodCall(data);
      calls.push(call);
      return StandardMethodCodec.instance.encodeSuccessEnvelope(reply(call));
    });
    return calls;
  }

  /** 复位整个框架状态(每个测试之后)。 */
  async reset(): Promise<void> {
    this.binding.debugResetForTest();
    defaultBinaryMessenger.debugReset();
    paragraphCache.clear();
    imageCache.clear();
    HardwareKeyboard.instance.clearState();
    FocusManager.instance.debugReset();
    TextInput.debugReset();
    await this.flushMicrotasks();
  }
}

let shared: WidgetTester | null = null;

/** 获取(并复位视图尺寸的)共享 tester。 */
export function tester(size: Size = new Size(800, 600), dpr = 1): WidgetTester {
  if (shared === null) shared = new WidgetTester(size, dpr);
  else shared.setViewSize(size, dpr);
  return shared;
}

/** 在每个 widget 测试结束后调用。 */
export async function resetTester(): Promise<void> {
  if (shared !== null) await shared.reset();
}
