// ext.d.ts — 扩展 JS 门面可见的宿主能力类型模板。
// ExtensionHost 为每个注册了模块的扩展生成 `ext:<name>` synthetic module,
// 其默认导出形状如下(由 engine/extension/native_module_registry 实现)。
declare module "ext:*" {
  /** 扩展注册的原生函数集合(builder_add_function 注册的名字)。*/
  export const native: Record<string, (...args: NativeArg[]) => NativeRet>;
  type NativeArg = number | boolean | string | ArrayBuffer | ArrayBufferView;
  type NativeRet = number | boolean | string | ArrayBuffer | Promise<ArrayBuffer> | void;
}

declare module "momo:services" {
  /** 与原生侧消息通道互通(docs/04 §5)。扩展 JS 门面与应用均可使用。*/
  export function send(channel: string, bytes: ArrayBuffer): Promise<ArrayBuffer>;
  export function setHandler(
    channel: string,
    handler: ((bytes: ArrayBuffer) => Promise<ArrayBuffer | void>) | null,
  ): void;
}
