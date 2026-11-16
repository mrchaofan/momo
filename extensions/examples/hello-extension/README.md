# hello-extension — 最小示例扩展

原生扩展的最小闭环(docs/07):一个普通的 Node 插件(纯 C, 只用 Node-API)+ JS 门面。它不碰原生 UI、纹理、平台视图,
所以不需要宿主能力表(`momo_host.h`), 与给 Node 写的插件没有区别。

```
hello-extension/
├── package.json         # 普通的 package.json, 没有 Momo 专有的声明
├── js/index.ts          # JS 门面: import 插件, 给它的函数加上类型
├── native/
│   ├── CMakeLists.txt   # momo_add_extension(ext_hello src/ext_hello.c)
│   └── src/ext_hello.c  # Node-API 模块(NAPI_MODULE_INIT)
└── build/ext_hello.node # 编译产物(不入库)
```

| 函数 | 说明 |
|---|---|
| `add(a, b)` | 同步:数字相加 |
| `greet(name)` | 同步:字符串往返(UTF-8) |
| `reverseBytes(bytes)` | 同步:返回字节逆序的新 `Uint8Array` |
| `hash(bytes)` | 异步:在 Node 的线程池上(`napi_async_work`)算 FNV-1a 64 位哈希, Promise 解析为 16 位十六进制字符串 |

原生侧(节选):

```c
// native/src/ext_hello.c
#include <node_api.h>

static napi_value Add(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2], result;
  double a, b;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2 || napi_get_value_double(env, argv[0], &a) != napi_ok ||
      napi_get_value_double(env, argv[1], &b) != napi_ok) {
    return Throw(env, "add(a, b) expects two numbers");
  }
  napi_create_double(env, a + b, &result);
  return result;
}

NAPI_MODULE_INIT() {
  /* 把 add / greet / reverseBytes / hash 挂到 exports 上 */
  return exports;
}
```

JS 门面:

```ts
// js/index.ts
import addon from "../build/ext_hello.node";   // 默认导出 = 插件的 exports
const native = addon as { add(a: number, b: number): number /* … */ };
export function add(a: number, b: number): number {
  return native.add(a, b);
}
```

独立构建(仓库内随 Momo 一起构建, target `ext_hello`):

```bash
cmake -S native -B native/out -DMomoExtSdk_DIR=<momo 构建目录>/sdk/lib/cmake/MomoExtSdk && cmake --build native/out
```
