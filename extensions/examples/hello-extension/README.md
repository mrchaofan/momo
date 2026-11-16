# hello-extension — 最小示例扩展

原生扩展的最小闭环(docs/07):一个普通的 Node 插件(纯 C, 只用 Node-API)+ JS 门面。它不碰原生 UI、纹理、平台视图,
所以不需要宿主能力表(`momo_host.h`), 与给 Node 写的插件没有区别。

```
hello-extension/
├── package.json         # 普通的 package.json(type: module, main: js/index.js, types: js/index.d.ts)
├── js/index.js          # JS 门面: 按相对路径 import 插件, 导出它的函数
├── js/index.d.ts        # 这些函数的类型
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

JS 门面(JS + 类型声明:这个包保持原样由 Node 加载, 运行时不执行 TS):

```js
// js/index.js
import addon from "../build/ext_hello.node";   // 相对路径;默认导出 = 插件的 exports
export const { add, greet, reverseBytes, hash } = addon;
```

```ts
// js/index.d.ts
export function add(a: number, b: number): number;
/* … */
```

用它的应用要让这个包保持原样留在 `node_modules` 里(docs/07 §2.2), `js/index.js` 才能按相对路径找到 `.node`。这是
应用自己的事, Momo 没有专门的配置项:在应用自己的构建配置里把它排除(不编译进 `main`), 打包时用 `momo.include`
把它带进应用包。

```json
"scripts": {
  "build": "esbuild src/main.ts --bundle --format=esm --platform=neutral '--external:momo:*' --external:@momo-examples/ext-hello --outfile=dist/main.js"
},
"momo": { "include": ["node_modules/@momo-examples/ext-hello/"] }
```

独立构建(仓库内随 Momo 一起构建, target `ext_hello`):

```bash
cmake -S native -B native/out -DMomoExtSdk_DIR=<momo 构建目录>/sdk/lib/cmake/MomoExtSdk && cmake --build native/out
```
