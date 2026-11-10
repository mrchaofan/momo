import { reportError } from "../foundation/assert";
import { MethodChannel, type MethodCall } from "./platform_channel";
import { TextRange, type TextEditingValue } from "./text_editing";

/** 接收平台文本输入事件的客户端(EditableText 实现)。 */
export interface TextInputClient {
  /**
   * 提交文本:replace 给出时替换该区间(输入法显式指定, UTF-16, 相对整段文本),
   * 否则替换 composing 区间(若有), 再否则替换选区。
   */
  commitText(text: string, replace?: TextRange | null): void;
  /** 更新预编辑文本;cursor 为预编辑文本内的光标(UTF-16);replace 语义同 commitText。 */
  updateComposing(text: string, cursor: number, replace?: TextRange | null): void;
  /** 结束预编辑(输入法取消)。 */
  endComposing(): void;
  /** 编辑命令:deleteBackward / insertNewline / moveLeft / selectAll ...(macOS 选择子去冒号)。 */
  performCommand(command: string): void;
  /** 连接被关闭(被其他 client 取代)。 */
  connectionClosed?(): void;
}

const channel = new MethodChannel("momo/textinput");

function invoke(method: string, args?: unknown): void {
  channel.invokeMethod(method, args).catch((e: unknown) => reportError(e, `in momo/textinput ${method}`));
}

/** 解析可选的 replaceStart/replaceEnd(两者都是非负整数且有序时才有效)。 */
function replaceRange(args: unknown): TextRange | null {
  const start = field(args, "replaceStart");
  const end = field(args, "replaceEnd");
  if (typeof start !== "number" || typeof end !== "number") return null;
  if (!Number.isInteger(start) || !Number.isInteger(end) || start < 0 || end < start) return null;
  return new TextRange(start, end);
}

function field(args: unknown, name: string): unknown {
  if (args instanceof Map) return args.get(name);
  if (args !== null && typeof args === "object") return (args as Record<string, unknown>)[name];
  return undefined;
}

/** 与平台输入法的一次连接。 */
export class TextInputConnection {
  #shown = false;
  /** @internal */ client: TextInputClient | null;

  /** @internal */
  constructor(client: TextInputClient) {
    this.client = client;
  }

  /** 是否仍是当前连接。 */
  get attached(): boolean {
    return TextInput.currentConnection === this;
  }

  /** 开始输入会话(显示输入法)。 */
  show(): void {
    if (!this.attached) return;
    this.#shown = true;
    invoke("show");
  }

  /** 结束输入会话。 */
  hide(): void {
    if (!this.#shown) return;
    this.#shown = false;
    invoke("hide");
  }

  /** 同步编辑状态给平台。 */
  setEditingState(value: TextEditingValue): void {
    if (!this.attached) return;
    const composing = value.isComposingRangeValid ? value.composing : null;
    invoke("setEditingState", {
      text: value.text,
      selectionBase: value.selection.baseOffset,
      selectionExtent: value.selection.extentOffset,
      composingStart: composing ? composing.start : -1,
      composingEnd: composing ? composing.end : -1,
    });
  }

  /** 光标矩形(物理像素, 相对窗口内容区左上角), 用于候选窗定位。 */
  setCaretRect(x: number, y: number, width: number, height: number): void {
    if (!this.attached) return;
    invoke("setCaretRect", [x, y, width, height]);
  }

  /** 关闭连接(若已 show 则发 hide)。 */
  close(): void {
    this.hide();
    if (TextInput.currentConnection === this) TextInput.currentConnection = null;
    this.client = null;
  }
}

/** 文本输入服务(momo/textinput 通道)。 */
export const TextInput = {
  /** @internal 当前连接。 */
  currentConnection: null as TextInputConnection | null,
  _initialized: false,

  /** 注册原生 → JS 的 handler(幂等)。 */
  ensureInitialized(): void {
    if (TextInput._initialized) return;
    TextInput._initialized = true;
    channel.setMethodCallHandler((call: MethodCall) => TextInput._handle(call));
  },

  /** 绑定客户端;已有连接会被关闭并通知其 client。 */
  attach(client: TextInputClient): TextInputConnection {
    TextInput.ensureInitialized();
    const old = TextInput.currentConnection;
    if (old !== null) {
      const oldClient = old.client;
      old.close();
      oldClient?.connectionClosed?.();
    }
    const conn = new TextInputConnection(client);
    TextInput.currentConnection = conn;
    return conn;
  },

  /** @internal */
  _handle(call: MethodCall): null {
    const client = TextInput.currentConnection?.client ?? null;
    if (client === null) return null;
    const args = call.args;
    switch (call.method) {
      case "commit": {
        const text = field(args, "text");
        client.commitText(typeof text === "string" ? text : "", replaceRange(args));
        break;
      }
      case "composing": {
        const text = field(args, "text");
        const cursor = field(args, "cursor");
        const t = typeof text === "string" ? text : "";
        client.updateComposing(t, typeof cursor === "number" ? cursor : t.length, replaceRange(args));
        break;
      }
      case "composingEnd":
        client.endComposing();
        break;
      case "command": {
        const command = field(args, "command");
        if (typeof command === "string") client.performCommand(command);
        break;
      }
      default:
        break;
    }
    return null;
  },

  /** 测试用:断开当前连接(不发消息)。 */
  debugReset(): void {
    TextInput.currentConnection = null;
  },
};
