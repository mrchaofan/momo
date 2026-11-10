import { MethodChannel } from "./platform_channel";

const channel = new MethodChannel("momo/clipboard");

/** 系统剪贴板(纯文本), 经 momo/clipboard 通道。 */
export const Clipboard = {
  /** 写入文本。 */
  async setData(text: string): Promise<void> {
    await channel.invokeMethod("setText", text);
  },
  /** 读取文本(无内容时 null)。 */
  async getData(): Promise<string | null> {
    const r = await channel.invokeMethod<unknown>("getText");
    return typeof r === "string" ? r : null;
  },
};
