import { MethodChannel } from "./platform_channel";

const channel = new MethodChannel("momo/url");

/** 用系统默认程序打开 URL(经 momo/url 通道)。 */
export async function openUrl(url: string): Promise<void> {
  await channel.invokeMethod("open", url);
}
