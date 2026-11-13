// examples/webview(多标签浏览器)测试的前置: 在示例模块执行 runApp 之前装好原生侧 mock。
// 单独成文件, 保证 import 顺序(ES 模块按导入顺序求值)。
import { MethodCall, StandardMethodCodec, defaultBinaryMessenger, utf8Decode, utf8Encode } from "../../packages/momo/src/index";
import { __mock } from "../mock/momo_ui";

// 启动窗口(隐式视图 0)的尺寸: 示例的第一个浏览器窗口接管它。
__mock.setPhysicalSize(1000, 700, 1);

export interface ViewCall { method: string; id: number; viewType?: string }
export const viewCalls: ViewCall[] = [];
export const webCommands: Record<string, unknown>[] = [];
export const openedUrls: string[] = [];

const codec = StandardMethodCodec.instance;
defaultBinaryMessenger.setMockMessageHandler("momo/platform_views", (data) => {
  const call: MethodCall = codec.decodeMethodCall(data);
  const args = (call.args ?? {}) as Record<string, unknown>;
  viewCalls.push({ method: call.method, id: args.id as number, viewType: args.viewType as string | undefined });
  return codec.encodeSuccessEnvelope(null);
});
// WebView 扩展的命令通道(JSON): 一律成功。
defaultBinaryMessenger.setMockMessageHandler("momo.webview", (data) => {
  const text = utf8Decode(new Uint8Array(data!));
  webCommands.push(JSON.parse(text) as Record<string, unknown>);
  return utf8Encode(JSON.stringify({ result: null })).slice().buffer;
});
// 输入法连接(地址栏聚焦时): 忽略。
defaultBinaryMessenger.setMockMessageHandler("momo/textinput", () => codec.encodeSuccessEnvelope(null));
// 系统浏览器打开(momo/url open): 记录。
defaultBinaryMessenger.setMockMessageHandler("momo/url", (data) => {
  const call: MethodCall = codec.decodeMethodCall(data);
  if (call.method === "open") openedUrls.push(String(call.args));
  return codec.encodeSuccessEnvelope(null);
});
