// 调用宿主通道的方法, 把"宿主不支持"当作空操作: 通道未注册(headless 测试运行器)或平台没有这个能力
// (错误码 unimplemented)时返回 null;其他错误报告后返回 null。
import { reportError } from "../foundation/assert";
import { MissingPluginException, PlatformException } from "./message_codecs";
import type { MethodChannel } from "./platform_channel";

export async function invokeHost<R>(channel: MethodChannel, method: string, args: unknown = null): Promise<R | null> {
  try {
    return await channel.invokeMethod<R>(method, args);
  } catch (e) {
    const unsupported = e instanceof MissingPluginException ||
      (e instanceof PlatformException && e.code === "unimplemented");
    if (!unsupported) reportError(e, `calling ${channel.name} ${method}`);
    return null;
  }
}
