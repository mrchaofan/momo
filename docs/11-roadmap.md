# 11 · 现状与未做

只记录现在的事实：什么验证过、什么没有、有哪些已知限制、哪些功能明确没做。代码怎么工作见 ARCHITECTURE.md。

## 已验证

- **macOS（Apple Silicon，macOS ≥ 13.5）**：构建、CTest（C++ 单测、golden、框架测试在 Node mock 与真实引擎 headless 两种宿主下、示例端到端、扩展端到端、SDK 独立构建）、`momo build` 打包 `.app` / DMG。
- 框架：View / Element / RenderObject、布局、手势、动画、焦点与快捷键、文本输入（IME）、精简版 Material、多窗口、原生菜单与托盘。
- 扩展：三方扩展示例（hello、texture）与内置扩展 `momo:webview`（WKWebView 平台视图）。

## 未验证

- **Windows 全部**：平台层（`embedder/windows/`）、D3D12 后端、`node_bindings_win.cc`、libnode 的 Windows 构建、`momo build` 的 Windows 分支、发布流程的 Windows 任务都**写了但从未在 Windows 上编译运行**。在 macOS 上只有不依赖 `<windows.h>` 的纯逻辑有单测；本机目前没有 mingw，连交叉编译检查也没有做。
- 需要真机手动验证、自动化测试覆盖不到的 macOS 路径：Metal → 软件回退、拖拽缩放时内容与窗口同步、IME 候选窗位置、多显示器切换；无 GPU / 显示器 / 端口的环境里会跳过 Metal、CVDisplayLink、inspector 网络用例。
- npm 上现成的原生包、用 node-gyp 构建的插件（按设计可用，没有拿真实的包测过）。

## 已知限制

- 没有热重启：一个进程一个 JS 运行时，改了代码要重启进程。入口不等待顶层 await。
- 没有 raster cache：每帧完整重放全部 picture。
- 平台视图：只有 macOS 的 Metal surface 支持；只支持平移与正向缩放；可命中区域按包围盒记录。
- 外部纹理只有 CPU 像素帧。
- `momo:webview`：仅 macOS；自定义协议的响应一次给出，不支持 Range；没有单独发布的类型包。
- GPU 设备丢失后只回退到软件渲染，不重建设备。
- macOS 产物是 ad-hoc 签名，没有 Developer ID 签名与公证。
- sanitizer 构建没有 V8，JS 相关路径测不到。
- `PlatformIntegration::displays()` 只用到了主显示器的刷新率。

## 没做

- 跑测试的 CI（只有发布流程，它不跑 `ctest`）；`CMakePresets.json`；格式化与 lint 配置。
- 无障碍（语义树 → AX / UIA）。
- 崩溃上报、性能浮层与帧时间轴诊断。
- V8 startup snapshot（冷启动只有 Node 的编译缓存）。
- Windows：平台视图、WebView2、非激活窗口、MSIX 打包。
- 文本的撤销 / 重做。
- Node 的 `AsyncLocalStorage` 上下文贯穿引擎回调。
