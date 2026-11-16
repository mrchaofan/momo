// examples/webview(多标签浏览器)测试的前置: 在示例模块执行 runApp 之前装好原生侧 mock。
// 单独成文件, 保证 import 顺序(ES 模块按导入顺序求值)。
import { MethodCall, StandardMethodCodec, defaultBinaryMessenger, utf8Decode } from "../../packages/momo/src/index";
import { webviewNative } from "../mock/ext_webview";
import { __mock } from "../mock/momo_ui";

// 启动窗口(隐式视图 0)的尺寸: 示例的第一个浏览器窗口接管它。
__mock.setPhysicalSize(1000, 700, 1);

export interface ViewCall { method: string; id: number; viewType?: string; params?: Record<string, unknown> }
export const viewCalls: ViewCall[] = [];
/** WebView 扩展: JS 发给原生插件(替身 test/mock/ext_webview.ts)的命令与协议应答。 */
export const webCommands = webviewNative.commands;
export const protocolResponses = webviewNative.protocolResponses;
export const openedUrls: string[] = [];
/** 应用菜单(momo/menu setApplicationMenu 最近一次发来的菜单项)。 */
export const applicationMenu: { items: Record<string, unknown>[] } = { items: [] };

const codec = StandardMethodCodec.instance;
defaultBinaryMessenger.setMockMessageHandler("momo/platform_views", (data) => {
  const call: MethodCall = codec.decodeMethodCall(data);
  const args = (call.args ?? {}) as Record<string, unknown>;
  const params = args.params instanceof Uint8Array ? (JSON.parse(utf8Decode(args.params)) as Record<string, unknown>) : undefined;
  viewCalls.push({ method: call.method, id: args.id as number, viewType: args.viewType as string | undefined, params });
  return codec.encodeSuccessEnvelope(null);
});
// 输入法连接(地址栏聚焦时): 忽略。
defaultBinaryMessenger.setMockMessageHandler("momo/textinput", () => codec.encodeSuccessEnvelope(null));
// 系统浏览器打开(momo/url open): 记录。
defaultBinaryMessenger.setMockMessageHandler("momo/url", (data) => {
  const call: MethodCall = codec.decodeMethodCall(data);
  if (call.method === "open") openedUrls.push(String(call.args));
  return codec.encodeSuccessEnvelope(null);
});
// 应用菜单(momo/menu): 记录最近一次的内容;测试以宿主的身份报告点击(itemClicked)。
defaultBinaryMessenger.setMockMessageHandler("momo/menu", (data) => {
  const call: MethodCall = codec.decodeMethodCall(data);
  const args = (call.args ?? {}) as Record<string, unknown>;
  if (call.method === "setApplicationMenu") applicationMenu.items = (args.items ?? []) as Record<string, unknown>[];
  return codec.encodeSuccessEnvelope(null);
});
