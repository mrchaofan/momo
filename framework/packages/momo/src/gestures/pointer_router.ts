import { reportError } from "../foundation/assert";
import type { Matrix4 } from "../painting/matrix4";
import type { PointerEvent, PointerSignalEvent } from "./events";

/** 指针路由回调。 */
export type PointerRoute = (event: PointerEvent) => void;

/** 按指针 id 把事件路由给识别器(对标 Flutter PointerRouter)。 */
export class PointerRouter {
  #routes = new Map<number, Map<PointerRoute, Matrix4 | null>>();
  #global = new Map<PointerRoute, Matrix4 | null>();

  addRoute(pointer: number, route: PointerRoute, transform: Matrix4 | null = null): void {
    let m = this.#routes.get(pointer);
    if (m === undefined) {
      m = new Map();
      this.#routes.set(pointer, m);
    }
    m.set(route, transform);
  }

  removeRoute(pointer: number, route: PointerRoute): void {
    const m = this.#routes.get(pointer);
    if (m === undefined) return;
    m.delete(route);
    if (m.size === 0) this.#routes.delete(pointer);
  }

  addGlobalRoute(route: PointerRoute, transform: Matrix4 | null = null): void {
    this.#global.set(route, transform);
  }
  removeGlobalRoute(route: PointerRoute): void {
    this.#global.delete(route);
  }

  /** 该指针是否还有路由。 */
  hasRoutes(pointer: number): boolean {
    return this.#routes.has(pointer);
  }

  /** 分发事件(路由表快照, 回调中移除的路由不再调用)。 */
  route(event: PointerEvent): void {
    const routes = this.#routes.get(event.pointer);
    if (routes !== undefined) {
      for (const [route, transform] of [...routes]) {
        if (routes.has(route)) this.#dispatch(route, event, transform);
      }
    }
    for (const [route, transform] of [...this.#global]) {
      if (this.#global.has(route)) this.#dispatch(route, event, transform);
    }
  }

  #dispatch(route: PointerRoute, event: PointerEvent, transform: Matrix4 | null): void {
    try {
      route(event.transformed(transform));
    } catch (e) {
      reportError(e, "while routing a pointer event");
    }
  }

  /** 测试用:清空。 */
  reset(): void {
    this.#routes.clear();
    this.#global.clear();
  }
}

/** 信号事件(滚轮)解析:同一事件只由第一个注册者处理(最内层的可滚动区域)。 */
export class PointerSignalResolver {
  #first: ((event: PointerSignalEvent) => void) | null = null;
  #event: PointerSignalEvent | null = null;

  register(event: PointerSignalEvent, callback: (event: PointerSignalEvent) => void): void {
    if (this.#first !== null) return;
    this.#event = event.original as PointerSignalEvent | null ?? event;
    this.#first = callback;
  }

  resolve(event: PointerSignalEvent): void {
    const cb = this.#first;
    if (cb === null) return;
    const e = this.#event ?? event;
    this.#first = null;
    this.#event = null;
    try {
      cb(e);
    } catch (err) {
      reportError(err, "while resolving a PointerSignalEvent");
    }
  }
}
