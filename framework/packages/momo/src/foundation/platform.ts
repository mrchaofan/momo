import * as services from "momo:services";

/** 目标平台。 */
export type TargetPlatform = "macos" | "windows" | "linux" | "headless";

let platformOverride: TargetPlatform | null = null;

/** 当前平台(可被测试覆盖)。 */
export function defaultTargetPlatform(): TargetPlatform {
  if (platformOverride !== null) return platformOverride;
  const p = services.platform;
  return p === "macos" || p === "windows" || p === "linux" ? p : "headless";
}

/** 测试用:覆盖平台(null 取消)。 */
export function debugOverrideTargetPlatform(platform: TargetPlatform | null): void {
  platformOverride = platform;
}

/** 是否为 Apple 平台(快捷键主修饰键为 Meta/Command)。 */
export function isApplePlatform(): boolean {
  return defaultTargetPlatform() === "macos";
}
