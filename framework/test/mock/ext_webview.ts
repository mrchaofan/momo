// 内置扩展 momo:webview 的原生插件(ext_webview.node)在 mock 宿主下的替身: 命令一律成功并记录,
// 测试以原生侧的身份发事件和协议请求。接口见 extensions/webview/native/src/ext_webview.mm。
import { utf8Decode } from "../../packages/momo/src/index";

interface Callbacks {
  onEvent(json: string): void;
  onProtocolRequest(frame: ArrayBuffer): void;
}

let callbacks: Callbacks | null = null;

export const webviewNative = {
  /** JS 发来的命令。 */
  commands: [] as Record<string, unknown>[],
  /** JS 协议处理函数的应答帧(uint32 LE 头长度 + 头 JSON + 正文)。 */
  protocolResponses: [] as { header: Record<string, unknown>; body: string }[],
  /** 原生 → JS: 一个事件。 */
  emitEvent(event: Record<string, unknown>): void {
    callbacks!.onEvent(JSON.stringify(event));
  },
  /** 原生 → JS: 页面对自定义协议地址的一次请求。 */
  requestProtocol(frame: ArrayBuffer): void {
    callbacks!.onProtocolRequest(frame);
  },
};

export default {
  attach(_host: bigint, options: Callbacks): void {
    callbacks = options;
  },
  command(json: string): Promise<string> {
    webviewNative.commands.push(JSON.parse(json) as Record<string, unknown>);
    return Promise.resolve(JSON.stringify({ result: null }));
  },
  respondToProtocolRequest(frame: ArrayBuffer): void {
    const bytes = new Uint8Array(frame);
    const length = new DataView(frame).getUint32(0, true);
    webviewNative.protocolResponses.push({
      header: JSON.parse(utf8Decode(bytes.subarray(4, 4 + length))) as Record<string, unknown>,
      body: utf8Decode(bytes.subarray(4 + length)),
    });
  },
};
