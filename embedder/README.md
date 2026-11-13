# embedder/ — 宿主层(C++)

设计文档:`docs/01-embedder.md`(装配与生命周期)、`docs/03-platform.md`(平台抽象与实现)。对标 Flutter 的
`shell/platform/<os>` + runner:拥有进程入口与原生消息循环,实现 engine 声明的平台契约(`engine/platform/`),
装配并驱动 engine。engine 不依赖本层。

| 目录 | 内容 |
|---|---|
| `common/` | 平台无关装配逻辑(可单测):`application.*`、`window_controller.*`、`app_config.*`、`asset_resolver.*`;宿主通道(engine 不可见, 在 hub 上注册处理器):`window_channel.*`(`momo/window`、`momo/app`)、`platform_channels.*`(`momo/clipboard`、`momo/url`、`momo/textinput`);embedder 自己的平台抽象(engine 不可见):`platform_integration.h`(工厂 + 原生循环)、`platform_window.h`(PlatformWindow/WindowDelegate/WindowSpec、可选窗口能力 `WindowCapability`)、`platform_services.h`、`text_input.h`、`window_types.h`、`display.h`;`engine_platform.h`(`platform::` = `momo::engine::platform` 的命名空间别名) |
| `macos/` | Cocoa 平台实现(`mac_window`、`mac_surface_{metal,software}`、`mac_vsync_displaylink`、`mac_text_input`、`mac_native_loop`、`mac_key_mapping`、`mac_platform_integration`)+ 入口(`main_mac.mm`、`mac_app_delegate`) |
| `windows/` | Win32 平台实现(`win_window`、`win_surface_software`、`win_vsync_dwm`、`win_text_input`、`win_services`、`win_native_loop`、`win_key_mapping`、`win_util`、`win_platform_integration`)+ 入口(`main_win.cc`、`win_app_delegate`、清单) |
| `headless/` | 无窗口宿主 `momo_headless`(离屏软件 surface;框架 TS 测试的引擎侧运行器) |

规则:
- 双端入口文件只做翻译(目标 <100 行);业务顺序全部在 `common/`。
- `common/` 只依赖平台抽象接口,平台实现由入口注入(单测用 `common/testing/mock_platform.h`)。
- 平台实现(`momo_embedder_{macos,windows}`)只链接 `momo_engine_platform`(接口)与系统 SDK,不链接引擎库。
- 命名空间:`momo::embedder`(common/headless)、`momo::embedder::{macos,windows}`;engine 契约类型写作 `platform::X`。
- 窗口外观与行为、平台服务都在本层扩展(PlatformWindow 可选方法 + `WindowCapability`、PlatformServices、宿主通道), 不改 engine。平台不支持的可选能力返回 `Unimplemented`, JS 可经 `getState` 查询能力。

## 状态

| 目录 | 状态 | 说明 |
|---|---|---|
| `common/` | ✅ | Application 状态机、退出流程(Quit/Exit)与有序关闭、WindowManager(所有窗口对等;最后一个窗口关闭不退出)、OS 主线程 EventLoop(base::NativeMessagePump)、WindowController、宿主通道(HostChannel: momo/app、momo/window…)、AppConfig、AssetResolver;gtest `momo_embedder_unittests`(mock 平台) |
| `macos/` | ✅ | 窗口、原生循环、CVDisplayLink vsync(按需启停)、鼠标/触控板/滚轮/键盘归一化(keycode→HID 全表)、NSTextInputClient、NSCursor、CAMetalLayer surface(失败回退 IOSurface 软件 surface;`MOMO_SOFTWARE_RENDERING=1` 强制软件);产物 `Momo.app`;gtest `momo_embedder_macos_unittests` |
| `windows/` | 🟡 已实现,**未在 Windows 上编译验证**;GPU(ANGLE)未实现 | Win32 窗口 + WndProc(Per-Monitor-V2 DPI)、隐藏消息窗口 NativeLoop、DwmFlush vsync、IMM32 输入法、GDI 软件 surface、剪贴板/OpenUrl;可选窗口能力只有定位与指针捕获(隐藏标题栏、系统拖动、移动循环、置顶、平台视图未实现);`win_platform_unittests.cc` 待在 Windows 上运行 |
| `headless/` | ✅ | `HeadlessSmoke`、`FrameworkTests.EngineHeadless` |

未创建:应用菜单/右键菜单接口(规划中)。

CMake target:`momo_embedder_common`、`momo_embedder_macos` / `momo_embedder_windows`、可执行 `momo_app_mac`(Momo.app)/ `momo_app_win`(Momo.exe)、`momo_headless`。

```bash
cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/debug && (cd out/debug && ctest)
out/debug/embedder/Momo.app/Contents/MacOS/Momo --bundle=path/to/bundle.js   # 或 --dev 无 bundle 启动
```
