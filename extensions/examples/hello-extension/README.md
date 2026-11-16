# hello-extension — 最小的三方扩展示例

一个普通的 Node 插件（纯 C，只用 Node-API）+ JS 门面。它不碰原生 UI、纹理、平台视图，所以不需要宿主能力表，与给 Node 写的插件没有区别。包的结构、怎么写、应用怎么使用和打包它，见 [docs/07-extension-system.md](../../../docs/07-extension-system.md) §2、§6。

| 函数 | 说明 |
|---|---|
| `add(a, b)` | 同步：数字相加 |
| `greet(name)` | 同步：字符串往返（UTF-8） |
| `reverseBytes(bytes)` | 同步：返回字节逆序的新 `Uint8Array` |
| `hash(bytes)` | 异步：在 Node 的线程池上（`napi_async_work`）算 FNV-1a 64 位哈希，Promise 解析为 16 位十六进制字符串 |

仓库内随 Momo 一起构建（target `ext_hello`，产物 `build/ext_hello.node`）。独立构建：

```bash
cmake -S native -B native/out -DMomoExtSdk_DIR=<momo 构建目录>/sdk/lib/cmake/MomoExtSdk && cmake --build native/out
```
