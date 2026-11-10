# hello-extension — 最小示例扩展

演示扩展机制的最小闭环(docs/07):注册 `ext:hello` 模块,暴露同步函数 `add(a,b)` 与异步函数 `sha256(bytes)`(WorkerPool + Promise)。

```
hello-extension/
├── package.json        # momo.extension 声明(id: "momo.example.hello")
├── js/index.ts         # JS 门面: import { native } from "ext:hello" 并导出友好 API
└── native/
    ├── CMakeLists.txt  # 产出 ext_hello.{dll,dylib}, 仅依赖 momo_ext_sdk
    └── src/ext_hello.cc# momo_ext_init: on_attach 中 register_module + add_function
```

原生侧骨架(示意):

```cpp
// native/src/ext_hello.cc
#include "momo/momo_extension.h"

namespace {
const MomoHostApi* g_host = nullptr;

void Add(void*, MomoCallInfoRef call) {
  double a = 0, b = 0;
  if (g_host->call->arg_double(call, 0, &a) != kMomoOk ||
      g_host->call->arg_double(call, 1, &b) != kMomoOk) {
    g_host->call->throw_error(call, "add(a, b) expects two numbers");
    return;
  }
  g_host->call->return_double(call, a + b);
}

void OnAttach(void*, const MomoHostApi* host, MomoEngineRef engine) {
  g_host = host;
  host->register_module(engine, "ext:hello",
      [](void*, MomoJsBuilderRef b) { g_host->builder_add_function(b, "add", Add, nullptr); },
      nullptr);
}
void OnDetach(void*) { g_host = nullptr; }
}  // namespace

MOMO_EXT_EXPORT bool momo_ext_init(uint32_t host_version, MomoExtension* out) {
  if (host_version < MOMO_EXT_API_VERSION) return false;
  out->id = "momo.example.hello";
  out->on_attach = OnAttach;
  out->on_detach = OnDetach;
  return true;
}
```

JS 门面:

```ts
// js/index.ts
import { native } from "ext:hello";
export const add = (a: number, b: number): number => native.add(a, b) as number;
```
