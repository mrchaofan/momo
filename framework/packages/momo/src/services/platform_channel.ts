import { defaultBinaryMessenger, type BinaryMessenger } from "./binary_messenger";
import { MethodCall, MissingPluginException, PlatformException, type MessageCodec, type MethodCodec } from "./message_codecs";
import { StandardMethodCodec } from "./standard_codec";

export { MethodCall, MissingPluginException, PlatformException } from "./message_codecs";

/** 基于 MessageCodec 的双向消息通道。 */
export class BasicMessageChannel<T> {
  readonly messenger: BinaryMessenger;
  constructor(readonly name: string, readonly codec: MessageCodec<T>, messenger?: BinaryMessenger) {
    this.messenger = messenger ?? defaultBinaryMessenger;
  }

  /** 发送消息并等待回复。 */
  async send(message: T | null): Promise<T | null> {
    const reply = await this.messenger.send(this.name, this.codec.encodeMessage(message));
    return this.codec.decodeMessage(reply);
  }

  /** 设置/清除接收原生消息的 handler。 */
  setMessageHandler(handler: ((message: T | null) => T | null | void | Promise<T | null | void>) | null): void {
    if (handler === null) {
      this.messenger.setMessageHandler(this.name, null);
      return;
    }
    this.messenger.setMessageHandler(this.name, async (data) => {
      const reply = await handler(this.codec.decodeMessage(data));
      return this.codec.encodeMessage(reply ?? null);
    });
  }
}

/** 方法调用通道(默认 StandardMethodCodec)。 */
export class MethodChannel {
  readonly messenger: BinaryMessenger;
  constructor(readonly name: string, readonly codec: MethodCodec = StandardMethodCodec.instance, messenger?: BinaryMessenger) {
    this.messenger = messenger ?? defaultBinaryMessenger;
  }

  /**
   * 调用原生方法。错误信封抛 {@link PlatformException};无原生 handler(回复 null)抛 {@link MissingPluginException}。
   */
  async invokeMethod<R = unknown>(method: string, args?: unknown): Promise<R | null> {
    const reply = await this.messenger.send(this.name, this.codec.encodeMethodCall(new MethodCall(method, args ?? null)));
    if (reply === null) {
      throw new MissingPluginException(`No implementation found for method ${method} on channel ${this.name}`);
    }
    return this.codec.decodeEnvelope(reply) as R | null;
  }

  /**
   * 设置/清除处理原生调用的 handler。handler 抛 PlatformException → 错误信封;其他异常 → code "error"。
   */
  setMethodCallHandler(handler: ((call: MethodCall) => unknown) | null): void {
    if (handler === null) {
      this.messenger.setMessageHandler(this.name, null);
      return;
    }
    this.messenger.setMessageHandler(this.name, async (data) => {
      let call: MethodCall;
      try {
        call = this.codec.decodeMethodCall(data);
      } catch (e) {
        return this.codec.encodeErrorEnvelope("error", e instanceof Error ? e.message : String(e));
      }
      try {
        const result: unknown = await handler(call);
        return this.codec.encodeSuccessEnvelope(result ?? null);
      } catch (e) {
        if (e instanceof PlatformException) return this.codec.encodeErrorEnvelope(e.code, e.platformMessage, e.details);
        return this.codec.encodeErrorEnvelope("error", e instanceof Error ? e.message : String(e));
      }
    });
  }
}
